#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <glib.h>
#include <string.h>
#include <iostream>
#include <algorithm>
#include "rtsp_to_jpeg.h"

using namespace std;

extern "C" {
    void goFrameCallbackBridge(unsigned char *data, int size, void *userData);
}

static void print_buffer_info(GstBuffer *buffer) {
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        cout << "Buffer size: " << map.size << " bytes" << endl;
        cout << "First 16 bytes: ";
        for (gsize i = 0; i < min((gsize)16, map.size); i++) {
            printf("%02x ", map.data[i]);
        }
        cout << endl;
        gst_buffer_unmap(buffer, &map);
    }
}

static GstFlowReturn new_sample(GstElement *sink, AppData *app) {
    cout << "=== NEW SAMPLE CALLBACK TRIGGERED ===" << endl;
    
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) {
        cerr << "Failed to get sample from appsink" << endl;
        return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        cerr << "Failed to get buffer from sample" << endl;
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (app->callback) {
            app->callback(map.data, map.size, app->user_data);
        } else if (app->user_data) {
            goFrameCallbackBridge(map.data, map.size, app->user_data);
        }
        gst_buffer_unmap(buffer, &map);
    }

    app->frame_count++;
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            cerr << "Error: " << err->message << endl;
            if (debug) cerr << "Debug info: " << debug << endl;
            g_error_free(err);
            g_free(debug);
            g_main_loop_quit((GMainLoop *)data);
            break;
        }
        case GST_MESSAGE_EOS:
            cout << "End of stream" << endl;
            g_main_loop_quit((GMainLoop *)data);
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(((AppData *)data)->pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                cout << "Pipeline state changed from " << gst_element_state_get_name(old_state)
                     << " to " << gst_element_state_get_name(new_state) << endl;
            }
            break;
        default:
            break;
    }
    return TRUE;
}

extern "C" {

// In create_pipeline():
AppData* create_pipeline() {
    cout << "Creating new pipeline..." << endl;
    
    AppData *app = (AppData *)calloc(1, sizeof(AppData)); 
    if (!app) {
        cerr << "Failed to allocate AppData" << endl;
        return NULL;
    }

    gst_init(NULL, NULL);
    
    // Create main loop first
    app->loop = g_main_loop_new(NULL, FALSE);
    if (!app->loop) {
        cerr << "Failed to create main loop" << endl;
        free(app);
        return NULL;
    }

    // Rest of pipeline creation...
    app->pipeline = gst_pipeline_new("h264-to-jpeg-pipeline");
    app->appsrc = gst_element_factory_make("appsrc", "h264-source");
    app->parser = gst_element_factory_make("h264parse", "h264-parser");
    app->decoder = gst_element_factory_make("avdec_h264", "h264-decoder");
    app->converter = gst_element_factory_make("videoconvert", "video-converter");
    app->scaler = gst_element_factory_make("videoscale", "video-scaler");
    app->jpegenc = gst_element_factory_make("jpegenc", "jpeg-encoder");
    app->appsink = gst_element_factory_make("appsink", "app-sink");

    if (!app->pipeline || !app->appsrc || !app->parser || !app->decoder || 
        !app->converter || !app->scaler || !app->jpegenc || !app->appsink) {
        cerr << "Failed to create pipeline elements" << endl;
        if (!app->pipeline) cerr << "Pipeline creation failed" << endl;
        if (!app->appsrc) cerr << "Appsrc creation failed" << endl;
        if (!app->parser) cerr << "Parser creation failed" << endl;
        if (!app->decoder) cerr << "Decoder creation failed" << endl;
        if (!app->converter) cerr << "Converter creation failed" << endl;
        if (!app->scaler) cerr << "Scaler creation failed" << endl;
        if (!app->jpegenc) cerr << "JPEG encoder creation failed" << endl;
        if (!app->appsink) cerr << "Appsink creation failed" << endl;
        
        free(app);
        return NULL;
    }

    // Configure appsrc
    g_object_set(G_OBJECT(app->appsrc),
        "stream-type", 0,
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        NULL);
    
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        NULL);
    gst_app_src_set_caps(GST_APP_SRC(app->appsrc), caps);
    gst_caps_unref(caps);

    // Configure scaler output resolution (1920x1080)
    GstCaps *scale_caps = gst_caps_new_simple("video/x-raw",
        "width", G_TYPE_INT, 1920,
        "height", G_TYPE_INT, 1080,
        NULL);
    
    // Create capsfilter for scaler
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "scaler-caps");
    g_object_set(G_OBJECT(capsfilter), "caps", scale_caps, NULL);
    gst_caps_unref(scale_caps);

    // Configure appsink
    g_object_set(G_OBJECT(app->appsink),
        "emit-signals", TRUE,
        "sync", FALSE,
        "drop", TRUE,
        NULL);
    
    GstCaps *sink_caps = gst_caps_new_simple("image/jpeg", NULL);
    gst_app_sink_set_caps(GST_APP_SINK(app->appsink), sink_caps);
    gst_caps_unref(sink_caps);
    
    g_signal_connect(app->appsink, "new-sample", G_CALLBACK(new_sample), app);

    // Build pipeline
    cout << "Building pipeline..." << endl;
    gst_bin_add_many(GST_BIN(app->pipeline), 
        app->appsrc, app->parser, app->decoder,
        app->converter, app->scaler, capsfilter, app->jpegenc, app->appsink, NULL);
        
    if (!gst_element_link_many(app->appsrc, app->parser, app->decoder,
                             app->converter, app->scaler, capsfilter, app->jpegenc, app->appsink, NULL)) {
        cerr << "Failed to link pipeline elements" << endl;
        gst_object_unref(app->pipeline);
        free(app);
        return NULL;
    }

    // Add bus watch
    GstBus *bus = gst_element_get_bus(app->pipeline);
    gst_bus_add_watch(bus, bus_call, app->loop);
    gst_object_unref(bus);

    app->loop = g_main_loop_new(NULL, FALSE);
    cout << "Pipeline created successfully" << endl;
    return app;
}

void set_frame_callback(AppData *app, FrameCallback callback, void *user_data) {
    cout << "Setting frame callback" << endl;
    app->callback = callback;
    app->user_data = user_data;
}

void push_buffer(AppData *app, unsigned char *data, int size) {
    cout << "Pushing buffer of size " << size << " bytes" << endl;
    cout << "First 16 bytes: ";
    for (int i = 0; i < min(16, size); i++) {
        printf("%02x ", data[i]);
    }
    cout << endl;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, data, size);
        gst_buffer_unmap(buffer, &map);
    } else {
        cerr << "Failed to map buffer for writing" << endl;
        gst_buffer_unref(buffer);
        return;
    }
    
    GST_BUFFER_PTS(buffer) = app->timestamp;
    GST_BUFFER_DTS(buffer) = app->timestamp;
    app->timestamp += GST_SECOND / 30;  // 30 fps
    
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(app->appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        cerr << "Failed to push buffer: " << ret << endl;
    }
}

void start_pipeline(AppData *app) {
    cout << "Starting pipeline..." << endl;
    
    GstStateChangeReturn ret = gst_element_set_state(app->pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        cerr << "Failed to pause pipeline" << endl;
        return;
    }
    
    ret = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        cerr << "Failed to start pipeline" << endl;
        return;
    }
    
    cout << "Pipeline started successfully" << endl;
    g_main_loop_run(app->loop);
}

void destroy_pipeline(AppData *app) {
    if (!app) return;
    
    cout << "Destroying pipeline..." << endl;
    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
    }
    if (app->loop) {
        g_main_loop_unref(app->loop);
    }
    free(app);
    cout << "Pipeline destroyed" << endl;
}

} // extern "C"