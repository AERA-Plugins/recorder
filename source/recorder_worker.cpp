/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <glib-unix.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace aera_recorder;

namespace {

struct Recorder {
  GMainLoop *loop = nullptr;
  GstElement *pipeline = nullptr;
  GstAppSrc *source = nullptr;
  uint8_t *shared = nullptr;
  size_t shared_bytes = 0;
  uint32_t frame_bytes = 0;
  uint32_t fps = 30;
  uint32_t last_sequence = 0;
  uint64_t frames = 0;
  bool started = false;
  bool finalizing = false;
};

bool Send(Recorder *recorder, Kind kind, const char *text = nullptr,
          uint32_t sequence = 0) {
  Message message;
  message.kind = kind;
  message.sequence = sequence;
  message.frames = recorder->frames;
  if (text) snprintf(message.text, sizeof(message.text), "%s", text);
  return send(4, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(message));
}

bool SafeOutput(const char *path) {
  constexpr char prefix[] = "/recordings/";
  if (!path || strncmp(path, prefix, sizeof(prefix) - 1) ||
      strlen(path) <= sizeof(prefix) - 1 || strlen(path) >= 500)
    return false;
  const char *leaf = path + sizeof(prefix) - 1;
  return !strchr(leaf, '/') && !strstr(leaf, "..") &&
         strlen(leaf) > 4 && !strcasecmp(leaf + strlen(leaf) - 4, ".mp4");
}

void DestroyPipeline(Recorder *recorder) {
  if (recorder->pipeline) {
    gst_element_set_state(recorder->pipeline, GST_STATE_NULL);
    gst_object_unref(recorder->pipeline);
  }
  recorder->pipeline = nullptr;
  recorder->source = nullptr;
  recorder->started = false;
}

gboolean BusMessage(GstBus *, GstMessage *message, gpointer data) {
  auto *recorder = static_cast<Recorder *>(data);
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    Send(recorder, Kind::kError,
         error ? error->message : "The video encoder stopped unexpectedly.");
    if (error) g_error_free(error);
    g_free(debug);
    g_main_loop_quit(recorder->loop);
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    Send(recorder, Kind::kDone, "Recording saved");
    g_main_loop_quit(recorder->loop);
  }
  return G_SOURCE_CONTINUE;
}

bool StartPipeline(Recorder *recorder, const Message &request) {
  if (recorder->started || !ValidDimensions(request) ||
      (request.fps != 30 && request.fps != 60) || !SafeOutput(request.text))
    return false;
  const uint64_t required =
      static_cast<uint64_t>(request.data_bytes) * kFrameSlots;
  if (required != recorder->shared_bytes) return false;

  GstElement *pipeline = gst_pipeline_new("aera-recorder");
  GstElement *source = gst_element_factory_make("appsrc", "source");
  GstElement *convert = gst_element_factory_make("videoconvert", "convert");
  GstElement *encoder = gst_element_factory_make("openh264enc", "encoder");
  GstElement *parser = gst_element_factory_make("h264parse", "parser");
  GstElement *muxer = gst_element_factory_make("mp4mux", "muxer");
  GstElement *sink = gst_element_factory_make("filesink", "sink");
  if (!pipeline || !source || !convert || !encoder || !parser || !muxer ||
      !sink) {
    if (pipeline) gst_object_unref(pipeline);
    return false;
  }

  GstCaps *caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT,
      static_cast<int>(request.width), "height", G_TYPE_INT,
      static_cast<int>(request.height), "framerate", GST_TYPE_FRACTION,
      static_cast<int>(request.fps), 1, nullptr);
  g_object_set(source, "caps", caps, "format", GST_FORMAT_TIME, "is-live", TRUE,
               "block", FALSE, nullptr);
  gst_caps_unref(caps);
  const guint bitrate = request.width >= 1000 ? 12000000U : 6500000U;
  g_object_set(encoder, "bitrate", bitrate, nullptr);
  g_object_set(muxer, "faststart", TRUE, nullptr);
  g_object_set(sink, "location", request.text, "sync", FALSE, nullptr);

  gst_bin_add_many(GST_BIN(pipeline), source, convert, encoder, parser, muxer,
                   sink, nullptr);
  if (!gst_element_link_many(source, convert, encoder, parser, muxer, sink,
                             nullptr)) {
    gst_object_unref(pipeline);
    return false;
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, BusMessage, recorder);
  gst_object_unref(bus);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    gst_object_unref(pipeline);
    return false;
  }
  recorder->pipeline = pipeline;
  recorder->source = GST_APP_SRC(source);
  recorder->frame_bytes = request.data_bytes;
  recorder->fps = request.fps;
  recorder->started = true;
  Send(recorder, Kind::kStatus, "Recording");
  return true;
}

bool PushFrame(Recorder *recorder, const Message &message) {
  if (!recorder->started || recorder->finalizing ||
      message.data_bytes != recorder->frame_bytes ||
      message.sequence != recorder->last_sequence + 1)
    return false;
  const uint32_t slot = message.sequence % kFrameSlots;
  const uint8_t *pixels = recorder->shared +
      static_cast<size_t>(slot) * recorder->frame_bytes;
  GstBuffer *buffer = gst_buffer_new_allocate(nullptr, recorder->frame_bytes,
                                               nullptr);
  if (!buffer || gst_buffer_fill(buffer, 0, pixels, recorder->frame_bytes) !=
                     recorder->frame_bytes) {
    if (buffer) gst_buffer_unref(buffer);
    return false;
  }
  GST_BUFFER_PTS(buffer) = message.timestamp_ns;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / recorder->fps;
  const GstFlowReturn flow = gst_app_src_push_buffer(recorder->source, buffer);
  if (flow != GST_FLOW_OK) return false;
  recorder->last_sequence = message.sequence;
  ++recorder->frames;
  return Send(recorder, Kind::kAck, nullptr, message.sequence);
}

gboolean Input(gint, GIOCondition condition, gpointer data) {
  auto *recorder = static_cast<Recorder *>(data);
  if (condition & (G_IO_HUP | G_IO_ERR)) {
    g_main_loop_quit(recorder->loop);
    return G_SOURCE_REMOVE;
  }
  Message message;
  const ssize_t count =
      recv(4, &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return G_SOURCE_CONTINUE;
  if (count != sizeof(message) || message.magic != kMagic ||
      !HasTextTerminator(message)) {
    g_main_loop_quit(recorder->loop);
    return G_SOURCE_REMOVE;
  }
  switch (message.kind) {
    case Kind::kStart:
      if (!StartPipeline(recorder, message)) {
        Send(recorder, Kind::kError, "Could not initialize the MP4 encoder.");
        g_main_loop_quit(recorder->loop);
        return G_SOURCE_REMOVE;
      }
      break;
    case Kind::kFrame:
      if (!PushFrame(recorder, message)) {
        Send(recorder, Kind::kError, "The recorder rejected a video frame.");
        g_main_loop_quit(recorder->loop);
        return G_SOURCE_REMOVE;
      }
      break;
    case Kind::kStop:
      if (recorder->started && !recorder->finalizing) {
        recorder->finalizing = true;
        Send(recorder, Kind::kStatus, "Finalizing MP4");
        gst_app_src_end_of_stream(recorder->source);
      }
      break;
    case Kind::kClose:
      g_main_loop_quit(recorder->loop);
      return G_SOURCE_REMOVE;
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

}  // namespace

int main() {
  if (fcntl(3, F_GETFD) < 0 || fcntl(4, F_GETFD) < 0) return 78;
  struct stat info{};
  if (fstat(3, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) > kMaximumFrameBytes * kFrameSlots)
    return 78;
  void *mapping = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ,
                       MAP_SHARED, 3, 0);
  if (mapping == MAP_FAILED) return 78;
  gst_init(nullptr, nullptr);
  Recorder recorder;
  recorder.shared = static_cast<uint8_t *>(mapping);
  recorder.shared_bytes = static_cast<size_t>(info.st_size);
  recorder.loop = g_main_loop_new(nullptr, FALSE);
  if (!recorder.loop) return 78;
  g_unix_fd_add(4, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), Input,
                &recorder);
  Send(&recorder, Kind::kStatus, "Encoder ready");
  g_main_loop_run(recorder.loop);
  DestroyPipeline(&recorder);
  g_main_loop_unref(recorder.loop);
  munmap(mapping, recorder.shared_bytes);
  return 0;
}

