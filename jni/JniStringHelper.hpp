#pragma once

#include <jni.h>
#include <string>

namespace tt {
class JniStringGuard {
public:
    JniStringGuard(JNIEnv* env, jstring jstr) : env_(env), jstr_(jstr), c_str_(nullptr) {
        if (jstr) {
            c_str_ = env->GetStringUTFChars(jstr, nullptr);
        }
    }
    ~JniStringGuard() {
        if (jstr_ && c_str_) {
            env_->ReleaseStringUTFChars(jstr_, c_str_);
        }
    }
    const char* get() const { return c_str_ ? c_str_ : ""; }
    std::string str() const { return get(); }

private:
    JNIEnv* env_;
    jstring jstr_;
    const char* c_str_;
};
}
