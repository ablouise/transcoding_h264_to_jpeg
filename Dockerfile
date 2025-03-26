# Build stage
FROM golang:1.24.1-bullseye AS builder

# Install GStreamer and build dependencies
RUN apt-get update && \
    apt-get install -y \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    libjpeg-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy go.mod and go.sum first for better caching
COPY go.mod go.sum ./
RUN go mod tidy

# Copy source code
COPY . .

# Build the application
RUN CGO_ENABLED=1 GOOS=linux go build -o h264-to-jpeg -v .

# Runtime stage
FROM debian:bullseye-slim

# Install runtime dependencies
RUN apt-get update && \
    apt-get install -y \
    libgstreamer1.0-0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libjpeg62-turbo \
    && rm -rf /var/lib/apt/lists/*

# Copy built binary from builder stage
COPY --from=builder /app/h264-to-jpeg /usr/local/bin/

# Create directories for input/output
RUN mkdir -p /data/h264 /data/output_jpegs
VOLUME ["/data/h264", "/data/output_jpegs"]

# Set environment variables
ENV GST_DEBUG=2
ENV LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu

# Set working directory
WORKDIR /data

# Run the application
CMD ["h264-to-jpeg"]