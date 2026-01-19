#ifndef SAMPLEBUFFER_H
#define SAMPLEBUFFER_H

#include <cstdint>
#include <cstring>

template <size_t BUFFER_SIZE>
class SampleBuffer {
private:
    float buffer[BUFFER_SIZE];
    size_t write_pos;
    size_t read_pos;
    size_t recorded_size;
    bool recording;
    bool playing;

public:
    SampleBuffer() : write_pos(0), read_pos(0), recorded_size(0), recording(false), playing(false) {
        memset(buffer, 0, sizeof(buffer));
    }

    void Init() {
        write_pos = 0;
        read_pos = 0;
        recorded_size = 0;
        recording = false;
        playing = false;
        memset(buffer, 0, sizeof(buffer));
    }

    void Record(bool enable) {
        recording = enable;
        if (!enable) {
            recorded_size = write_pos;
        }
    }

    void Play(bool enable) {
        playing = enable;
    }

    bool IsRecording() const {
        return recording;
    }

    bool IsPlaying() const {
        return playing;
    }

    void Write(float sample) {
        if (recording) {
            buffer[write_pos] = sample;
            write_pos = (write_pos + 1) % BUFFER_SIZE;
            if (recorded_size < BUFFER_SIZE) {
                recorded_size++;
            }
        }
    }

    float ReadLooping() {
        if (!playing || recorded_size == 0) {
            return 0.0f;
        }

        float sample = buffer[read_pos];
        read_pos = (read_pos + 1) % recorded_size;
        return sample;
    }

    size_t GetCurrentRecordedSize() const {
        return recorded_size;
    }
};

#endif // SAMPLEBUFFER_H