#pragma once

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Camera;

class OwnerFaceProbe {
public:
    void Start(Camera& camera);
    void RequestEnroll();
    void RequestRecognize();
    void RequestDeleteLast();

private:
    static void Task(void* arg);
    Camera* camera_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> enroll_requested_{false};
    std::atomic<bool> recognize_requested_{false};
    std::atomic<bool> delete_requested_{false};
};
