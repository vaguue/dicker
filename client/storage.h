#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
  #include <winsock2.h>   // must precede windows.h
  #include <windows.h>
  #include <vss.h>
  #include <vswriter.h>
  #include <vsbackup.h>
  // link: vssapi ole32 oleaut32
#endif

// Counting semaphore used to cap simultaneous disk reads across all reader
// threads. This is the `diskConcurrency` knob: there is one reader thread per
// worker (SPSC requires it), but only this many may be reading at once.
struct Semaphore {
  std::atomic<int> permits;

  explicit Semaphore(int n) : permits(n) {
  }

  void acquire() {
    for (;;) {
      int p = this->permits.load(std::memory_order_acquire);
      while (p > 0) {
        if (this->permits.compare_exchange_weak(p, p - 1,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
          return;
        }
      }
      this->permits.wait(0, std::memory_order_acquire);
    }
  }

  void release() {
    this->permits.fetch_add(1, std::memory_order_release);
    this->permits.notify_one();
  }
};

// Shared, one instance for the whole run. Owns the disk-concurrency limiter and
// (on Windows) a single Volume Shadow Copy snapshot: the snapshot is created
// once over the backup volume so locked/open files can be read consistently,
// and resolve() rewrites a live path to its shadow-device equivalent. If VSS is
// unavailable (not elevated, service off, non-NTFS), snapshot() fails soft and
// resolve() returns the live path — the tool still runs, just without snapshot
// consistency.
struct Storage {
  Semaphore diskLimit;

#ifdef _WIN32
  IVssBackupComponents* backup = nullptr;
  VSS_ID snapshotId{};
  bool active = false;
  std::wstring volumeRoot;    // e.g. L"C:\\"
  std::wstring deviceObject;  // e.g. L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopyN"
#endif

  explicit Storage(size_t diskConcurrency)
    : diskLimit(diskConcurrency < 1 ? 1 : static_cast<int>(diskConcurrency)) {
  }

  ~Storage() {
    this->teardown();
  }

#ifndef _WIN32
  bool snapshot(const char*) {
    return false;
  }

  void teardown() {
  }

  std::string resolve(const char* path) {
    return std::string(path);
  }
#else
  // Snapshot the volume that `anyRootPath` lives on. Returns true if a shadow
  // copy is active; false means fall back to live reads.
  bool snapshot(const char* anyRootPath) {
    std::wstring wroot = toWide(anyRootPath);

    wchar_t volume[MAX_PATH];
    if (!GetVolumePathNameW(wroot.c_str(), volume, MAX_PATH)) {
      return false;
    }
    this->volumeRoot = volume;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      return false;
    }

    if (FAILED(CreateVssBackupComponents(&this->backup)) || this->backup == nullptr) {
      CoUninitialize();
      return false;
    }

    IVssAsync* async = nullptr;
    VSS_ID setId{};

    bool ok =
      SUCCEEDED(this->backup->InitializeForBackup()) &&
      SUCCEEDED(this->backup->SetBackupState(false, false, VSS_BT_COPY, false)) &&
      SUCCEEDED(this->backup->StartSnapshotSet(&setId)) &&
      SUCCEEDED(this->backup->AddToSnapshotSet(volume, GUID_NULL, &this->snapshotId));

    if (ok && SUCCEEDED(this->backup->PrepareForBackup(&async)) && async != nullptr) {
      ok = SUCCEEDED(async->Wait());
      async->Release();
      async = nullptr;
    }
    else {
      ok = false;
    }

    if (ok && SUCCEEDED(this->backup->DoSnapshotSet(&async)) && async != nullptr) {
      ok = SUCCEEDED(async->Wait());
      async->Release();
      async = nullptr;
    }
    else {
      ok = false;
    }

    VSS_SNAPSHOT_PROP props{};
    if (ok && SUCCEEDED(this->backup->GetSnapshotProperties(this->snapshotId, &props))) {
      this->deviceObject = props.m_pwszSnapshotDeviceObject;
      VssFreeSnapshotProperties(&props);
      this->active = true;
      return true;
    }

    this->teardown();
    return false;
  }

  void teardown() {
    if (this->backup != nullptr) {
      this->backup->Release();
      this->backup = nullptr;
    }
    if (this->active || this->deviceObject.empty() == false) {
      CoUninitialize();
    }
    this->active = false;
  }

  // Rewrite a live path onto the shadow device: replace the volume root prefix
  // (e.g. "C:\") with the snapshot device object, keeping the rest of the path.
  std::string resolve(const char* path) {
    if (!this->active) {
      return std::string(path);
    }

    std::wstring w = toWide(path);
    if (w.size() >= this->volumeRoot.size() &&
        _wcsnicmp(w.c_str(), this->volumeRoot.c_str(),
                  static_cast<int>(this->volumeRoot.size())) == 0) {
      std::wstring shadow = this->deviceObject + L"\\" + w.substr(this->volumeRoot.size());
      return toUtf8(shadow);
    }

    return std::string(path);
  }

  static std::wstring toWide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) {
      MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    }
    return w;
  }

  static std::string toUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) {
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    }
    return s;
  }
#endif
};
