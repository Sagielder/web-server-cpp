#pragma once
#include <unistd.h>
class MyFileDescriptor {

private:
int m_fd;
public:

    // Delete the copy constructor
    MyFileDescriptor(const MyFileDescriptor&) = delete;

    // Delete the copy assignment operator
    MyFileDescriptor& operator=(const MyFileDescriptor&) = delete;



    void Destroy() {
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
    }
    MyFileDescriptor(const int& fd) {
        m_fd = fd;
    }

    ~MyFileDescriptor() {
        Destroy();
    }

    int GetFileDescriptor() const {
        return m_fd;
    }

    MyFileDescriptor(MyFileDescriptor&& other) noexcept : m_fd(other.GetFileDescriptor()) {
        other.m_fd = -1;
    }

    MyFileDescriptor& operator=(MyFileDescriptor&& other) noexcept {
        if (this != &other) {
            this->Destroy();
            m_fd = other.GetFileDescriptor();
            other.m_fd = -1;
        }
        return *this;
    }
};