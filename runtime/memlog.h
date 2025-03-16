#ifndef __KS381__MEMLOG__H__INCLUDED__
#define __KS381__MEMLOG__H__INCLUDED__

#ifdef __cplusplus

#include <atomic>
#include <chrono>
#include <forward_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace ks381 {
  class MemLogger {
    private:
      struct TimestampedEntry {
        int mTid;
        uint64_t mTimestamp;
        std::string mEntry;

        bool operator<(const TimestampedEntry &other) const {
          return mTimestamp < other.mTimestamp;
        }

        bool operator>(const TimestampedEntry &other) const {
          return mTimestamp > other.mTimestamp;
        }

        TimestampedEntry(const TimestampedEntry &other) {
          mTid = other.mTid;
          mTimestamp = other.mTimestamp;
          mEntry = other.mEntry;
        }

        TimestampedEntry(const int &tid, const uint64_t &timestamp, std::string &entry) {
          mTid = tid;
          mTimestamp = timestamp;
          mEntry.swap(entry);
        }
      };

      int mNextThreadId = 1;
      std::mutex mListLock;
      std::forward_list<std::vector<TimestampedEntry>> mThreadLogs;

      static thread_local std::vector<TimestampedEntry> *sThreadEntry;
      static thread_local int sTid;
      // This is a raw pointer
      // because cilk shuts down AFTER _Exit()
      static MemLogger *sInstance;
      
      MemLogger() = default;


    public:

      // This is not how you do singleton, but hacked in
      // because cilk shuts down AFTER _Exit()
      static inline void init_instance() {
        if (!sInstance) {
            sInstance = new MemLogger();
        }
      }

      static inline void deinit_instance() {
        delete sInstance;
        sInstance = nullptr;
      }

      static inline MemLogger* instance() {
        return sInstance;
      }

      inline void log(const uint64_t &entryTime, std::string &entry) {
        if (sThreadEntry == nullptr) {
          std::lock_guard<std::mutex> lock(mListLock);
          mThreadLogs.emplace_front();
          sThreadEntry = &mThreadLogs.front();
          sTid = mNextThreadId++;
        }

        sThreadEntry->emplace_back(sTid, entryTime, entry);
      }

      template <typename... Args>
      static inline void logf(const char *const format, Args... args) {
        uint64_t entryTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        // Get the size of what would have been generated; add 1 for NULL termination
        size_t entryLength = snprintf(nullptr, 0, format, args ...) + 1;
        std::string entryString;
        if (entryLength <= 0) {
          entryString = std::string("Bad snprintf!");
        } else {
          std::unique_ptr<char []> entryCString(new char[entryLength]);
          snprintf(entryCString.get(), entryLength, format, args...);
          entryString = std::string(entryCString.get());
        }

        instance()->log(entryTime, entryString);
      }

      void dumpLog(const char *const fileName, const int &entriesPerThread);
      void dumpLog(std::ostream &out, const int &entriesPerThread);
      void dumpLog(const int &entriesPerThread);

      void rawDumpLog(const char *const fileName, const int &entriesPerThread);
      void rawDumpLog(std::ostream &out, const int &entriesPerThread);
      void rawDumpLog(const int &entriesPerThread);

      void dumpYamlLog(const char *const fileName, const int &entriesPerThread);
      void dumpYamlLog(std::ostream &out, const int &entriesPerThread);
      void dumpYamlLog(const int &entriesPerThread);

      static bool valid;

      ~MemLogger() {
        int val = -1;
        printf("Dumping log; %d threads\n", mNextThreadId - 1);
        rawDumpLog(val);
        MemLogger::valid = false;
      }
  };
};

#define EXTERN_C extern "C"

#else

#define EXTERN_C

#endif

EXTERN_C void* memlogger_instance();
EXTERN_C void memlogger_logf(const char *const format, ...);
EXTERN_C void memlogger_raw_dump_log();
EXTERN_C void memlogger_init();
EXTERN_C void memlogger_deinit();


#endif