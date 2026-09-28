#pragma once

// Lock de leitores e escritor do Database e dos planos de projeção (ADR-027).
//
// O `std::shared_mutex` não serve do jeito que vem, nas duas plataformas:
//
// - Windows (MinGW): passa pelo rwlock da winpthreads, que sob contenção
//   devolve erro em `lock_shared` -- a asserção `__ret == 0` da libstdc++
//   disparou no preset sanitizers, até com leitores só (modb.concurrency_smoke,
//   8 threads). Aqui: o SRWLOCK do sistema, o que o `std::shared_mutex` do
//   MSVC usa.
// - Linux (glibc): o rwlock padrão prefere leitores. Com leitores em laço que
//   se sobrepõem, o escritor nunca consegue o lock -- modb.concurrency_stress
//   ficou 20 minutos parado sob o TSan. Aqui: o rwlock da glibc com preferência
//   ao escritor (PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP), que exige leitura
//   não recursiva -- o ReadGuard do Database já nunca toma a leitura duas vezes
//   na mesma thread.
// - Outros: `std::shared_mutex`.
//
// Use-o no lugar de `std::shared_mutex` no motor.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__GLIBC__) || defined(__linux__)
#include <pthread.h>
#else
#include <shared_mutex>
#endif

namespace modb::object {

class RwLock {
public:
    RwLock(const RwLock&) = delete;
    RwLock& operator=(const RwLock&) = delete;

#if defined(_WIN32)
    RwLock() = default;
    void lock() noexcept { AcquireSRWLockExclusive(&lock_); }
    void unlock() noexcept { ReleaseSRWLockExclusive(&lock_); }
    void lock_shared() noexcept { AcquireSRWLockShared(&lock_); }
    void unlock_shared() noexcept { ReleaseSRWLockShared(&lock_); }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
#elif defined(__GLIBC__) || defined(__linux__)
    RwLock() noexcept {
        pthread_rwlockattr_t attr;
        pthread_rwlockattr_init(&attr);
#ifdef __GLIBC__
        // É um valor de enum, não macro: `#ifdef` nele seria sempre falso (e o
        // lock ficava com a preferência padrão, aos leitores).
        pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
        pthread_rwlock_init(&lock_, &attr);
        pthread_rwlockattr_destroy(&attr);
    }
    ~RwLock() { pthread_rwlock_destroy(&lock_); }
    void lock() noexcept { pthread_rwlock_wrlock(&lock_); }
    void unlock() noexcept { pthread_rwlock_unlock(&lock_); }
    void lock_shared() noexcept { pthread_rwlock_rdlock(&lock_); }
    void unlock_shared() noexcept { pthread_rwlock_unlock(&lock_); }

private:
    pthread_rwlock_t lock_;
#else
    RwLock() = default;
    void lock() { lock_.lock(); }
    void unlock() { lock_.unlock(); }
    void lock_shared() { lock_.lock_shared(); }
    void unlock_shared() { lock_.unlock_shared(); }

private:
    std::shared_mutex lock_;
#endif
};

} // namespace modb::object
