package main

/*
#cgo pkg-config: gstreamer-1.0 gstreamer-app-1.0

#include "rtsp_to_jpeg.h"

// Forward declaration of the Go callback
extern void goFrameCallbackBridge(unsigned char *data, int size, void *userData);
*/
import "C"
import (
	"fmt"
	"io/ioutil"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"
)

type H264ToJPEGConverter struct {
	converter *C.AppData
	frameCB   func([]byte, int) error
	frameNum  int
	mu        sync.Mutex
	wg        sync.WaitGroup
}

//export goFrameCallbackBridge
func goFrameCallbackBridge(data *C.uchar, size C.int, userData unsafe.Pointer) {
	converter := (*H264ToJPEGConverter)(userData)
	converter.mu.Lock()
	defer converter.mu.Unlock()

	goData := C.GoBytes(unsafe.Pointer(data), size)
	log.Printf("JPEG callback received frame %d (%d bytes)", converter.frameNum, len(goData))

	if converter.frameCB != nil {
		if len(goData) == 0 {
			log.Println("Warning: Received empty frame data")
			return
		}
		err := converter.frameCB(goData, converter.frameNum)
		if err != nil {
			log.Printf("Frame callback error: %v", err)
		}
	}
	converter.frameNum++
}

func NewH264ToJPEGConverter(frameCB func([]byte, int) error) (*H264ToJPEGConverter, error) {
	converter := C.create_pipeline()
	if converter == nil {
		return nil, fmt.Errorf("failed to create pipeline")
	}

	h := &H264ToJPEGConverter{
		converter: converter,
		frameCB:   frameCB,
	}

	C.set_frame_callback(converter, C.FrameCallback(C.goFrameCallbackBridge), unsafe.Pointer(h))

	return h, nil
}

func (h *H264ToJPEGConverter) Start() {
	h.wg.Add(1)
	go func() {
		defer h.wg.Done()

		// Add a small delay to ensure everything is initialized
		time.Sleep(500 * time.Millisecond)

		// Use runtime.LockOSThread() for CGO calls
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()

		C.start_pipeline(h.converter)
	}()
}

func (h *H264ToJPEGConverter) PushFrame(frame []byte) {
	if len(frame) == 0 {
		return
	}

	// Make a copy of the data that will be owned by C
	cData := C.CBytes(frame)
	defer C.free(cData)

	// Print debug info
	log.Printf("Pushing frame %d (%d bytes)", h.frameNum, len(frame))
	if len(frame) > 16 {
		log.Printf("First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7],
			frame[8], frame[9], frame[10], frame[11], frame[12], frame[13], frame[14], frame[15])
	}

	C.push_buffer(h.converter, (*C.uchar)(cData), C.int(len(frame)))
}

func (h *H264ToJPEGConverter) Close() {
	C.destroy_pipeline(h.converter)
	h.wg.Wait()
}

func readH264Frames(folder string) ([][]byte, error) {
	files, err := ioutil.ReadDir(folder)
	if err != nil {
		return nil, fmt.Errorf("failed to read directory: %v", err)
	}

	// Filter and sort .h264 files
	var frameFiles []string
	for _, file := range files {
		if strings.HasPrefix(file.Name(), "frame_") && strings.HasSuffix(file.Name(), ".h264") {
			frameFiles = append(frameFiles, file.Name())
		}
	}

	// Sort files numerically
	sort.Slice(frameFiles, func(i, j int) bool {
		num1, _ := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(frameFiles[i], "frame_"), ".h264"))
		num2, _ := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(frameFiles[j], "frame_"), ".h264"))
		return num1 < num2
	})

	// Read all frames
	var frames [][]byte
	for _, filename := range frameFiles {
		path := filepath.Join(folder, filename)
		data, err := ioutil.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("failed to read file %s: %v", path, err)
		}
		frames = append(frames, data)
	}

	return frames, nil
}

func main() {
	// Set GStreamer debug level
	os.Setenv("GST_DEBUG", "2")

	// Create output directory
	err := os.MkdirAll("output_jpegs", 0755)
	if err != nil {
		log.Fatalf("Failed to create output directory: %v", err)
	}

	// Create converter with frame handler
	converter, err := NewH264ToJPEGConverter(func(data []byte, frameNum int) error {
		if len(data) < 100 { // JPEGs should be at least 100 bytes
			return fmt.Errorf("suspiciously small JPEG frame (%d bytes)", len(data))
		}

		filename := fmt.Sprintf("output_jpegs/frame_%04d.jpg", frameNum)
		err := os.WriteFile(filename, data, 0644)
		if err != nil {
			return fmt.Errorf("failed to save frame %d: %w", frameNum, err)
		}
		log.Printf("Successfully saved frame %d to %s (%d bytes)", frameNum, filename, len(data))
		return nil
	})
	if err != nil {
		log.Fatal(err)
	}
	defer converter.Close()

	// Start the pipeline
	converter.Start()
	log.Println("Pipeline started - waiting 1 second for initialization")
	time.Sleep(1 * time.Second)

	// Read H.264 frames from folder
	frames, err := readH264Frames("./h264/")
	if err != nil {
		log.Fatalf("Failed to read H.264 frames: %v", err)
	}

	// Push frames to pipeline
	for i, frame := range frames {
		converter.PushFrame(frame)
		log.Printf("Pushed frame %d (%d bytes)", i, len(frame))
		time.Sleep(33 * time.Millisecond) // ~30fps
	}

	// Wait for processing to complete
	time.Sleep(5 * time.Second)
	log.Println("Processing complete")
}
