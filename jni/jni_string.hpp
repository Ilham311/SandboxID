#pragma once
#include <jni.h>
#include <cstddef>

class JniString {
    JNIEnv *env_;
    jstring jstr_;
    const char *utf_;
public:
    JniString(JNIEnv *env, jstring str)
        : env_(env), jstr_(str), utf_(nullptr) {
        if (jstr_) utf_ = env_->GetStringUTFChars(jstr_, nullptr);
    }

    ~JniString() {
        if (utf_ && jstr_) env_->ReleaseStringUTFChars(jstr_, utf_);
    }

    JniString(const JniString &) = delete;
    JniString &operator=(const JniString &) = delete;

    const char *c_str() const { return utf_; }
    explicit operator bool() const { return utf_ != nullptr; }
};
