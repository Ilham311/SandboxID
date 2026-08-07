// ternak_tt v2.2 — tt_scoped.hpp
// P1-A: RAII wrappers untuk JNI resources (UTF chars + local refs).
//
// Refs:
//   - https://developer.android.com/ndk/guides/jni-tips
//   - https://android-developers.googleblog.com/2011/11/jni-local-reference-changes-in-ics.html
//   - https://github.com/android/ndk/wiki/JNI
//
// Setiap GetStringUTFChars / NewStringUTF / FindClass / GetObjectClass yang
// tidak dibarengi ReleaseStringUTFChars / DeleteLocalRef adalah leak lokal.
// Di postAppSpecialize, local-ref frame sangat kecil (16 default) sehingga
// leak sekecil apa pun dapat menyebabkan JNI ERROR (local reference table
// overflow). ScopedUtfChars + ScopedLocalRef membuat hygiene otomatis.

#pragma once

#include <jni.h>
#include <string>
#include <string_view>
#include <utility>

namespace tt {

// RAII wrapper untuk GetStringUTFChars/ReleaseStringUTFChars.
// Ambil pointer C sekali, release otomatis di destructor.
class ScopedUtfChars {
public:
    ScopedUtfChars(JNIEnv* env, jstring s) noexcept
        : env_(env), jstr_(s), cstr_(nullptr) {
        if (env_ && jstr_) cstr_ = env_->GetStringUTFChars(jstr_, nullptr);
    }

    ~ScopedUtfChars() noexcept {
        if (cstr_ && env_ && jstr_) env_->ReleaseStringUTFChars(jstr_, cstr_);
    }

    ScopedUtfChars(const ScopedUtfChars&) = delete;
    ScopedUtfChars& operator=(const ScopedUtfChars&) = delete;

    ScopedUtfChars(ScopedUtfChars&& o) noexcept
        : env_(o.env_), jstr_(o.jstr_), cstr_(o.cstr_) {
        o.env_ = nullptr; o.jstr_ = nullptr; o.cstr_ = nullptr;
    }

    ScopedUtfChars& operator=(ScopedUtfChars&& o) noexcept {
        if (this != &o) {
            this->~ScopedUtfChars();
            env_ = o.env_; jstr_ = o.jstr_; cstr_ = o.cstr_;
            o.env_ = nullptr; o.jstr_ = nullptr; o.cstr_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] const char* c_str() const noexcept {
        return cstr_ ? cstr_ : "";
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return cstr_ ? std::string_view(cstr_) : std::string_view{};
    }
    [[nodiscard]] bool valid() const noexcept { return cstr_ != nullptr; }

private:
    JNIEnv*     env_;
    jstring     jstr_;
    const char* cstr_;
};

// RAII wrapper untuk local reference apa pun (jstring / jclass / jobject).
// Setelah FindClass / GetObjectClass / NewStringUTF harus dilepas kalau
// belum dikembalikan ke JVM.
template <typename T>
class ScopedLocalRef {
public:
    ScopedLocalRef() noexcept : env_(nullptr), ref_(nullptr) {}
    ScopedLocalRef(JNIEnv* env, T ref) noexcept : env_(env), ref_(ref) {}

    ~ScopedLocalRef() noexcept { reset(); }

    ScopedLocalRef(const ScopedLocalRef&) = delete;
    ScopedLocalRef& operator=(const ScopedLocalRef&) = delete;

    ScopedLocalRef(ScopedLocalRef&& o) noexcept
        : env_(o.env_), ref_(o.ref_) { o.env_ = nullptr; o.ref_ = nullptr; }

    ScopedLocalRef& operator=(ScopedLocalRef&& o) noexcept {
        if (this != &o) {
            reset();
            env_ = o.env_; ref_ = o.ref_;
            o.env_ = nullptr; o.ref_ = nullptr;
        }
        return *this;
    }

    void reset(T new_ref = nullptr) noexcept {
        if (ref_ && env_) env_->DeleteLocalRef(ref_);
        ref_ = new_ref;
    }

    [[nodiscard]] T   get()   const noexcept { return ref_; }
    [[nodiscard]] T   release() noexcept { T r = ref_; ref_ = nullptr; return r; }
    [[nodiscard]] explicit operator bool() const noexcept { return ref_ != nullptr; }

private:
    JNIEnv* env_;
    T       ref_;
};

// Helper: unconditionally clear any pending JNI exception.
// Per JNI spec, ExceptionClear is a no-op when no exception is pending, so
// it is always safe to call. We keep this a thin wrapper so callers stay
// self-documenting ("we just did something that might have thrown").
inline void clear_pending_exception(JNIEnv* env) noexcept {
    if (env) env->ExceptionClear();
}

}  // namespace tt
