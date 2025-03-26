#ifndef RTSP_TO_JPEG_H
#define RTSP_TO_JPEG_H

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

typedef void (*FrameCallback)(unsigned char *data, int size, void *userData);

typedef struct {
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *parser;
    GstElement *decoder;
    GstElement *converter;
    GstElement *scaler;  // Added scaler element
    GstElement *jpegenc;
    GstElement *appsink;
    GMainLoop *loop;
    FrameCallback callback;
    void *user_data;
    int frame_count;
    guint64 timestamp;
} AppData;

#ifdef __cplusplus
extern "C" {
#endif

AppData* create_pipeline();
void set_frame_callback(AppData *app, FrameCallback callback, void *user_data);
void push_buffer(AppData *app, unsigned char *data, int size);
void start_pipeline(AppData *app);
void destroy_pipeline(AppData *app);

#ifdef __cplusplus
}
#endif

#endif // RTSP_TO_JPEG_H