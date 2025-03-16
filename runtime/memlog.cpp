#include "memlog.h"

#include <cstdarg>
#include <fstream>
#include <queue>

namespace ks381 {
  thread_local std::vector<MemLogger::TimestampedEntry> *MemLogger::sThreadEntry = nullptr;
  thread_local int MemLogger::sTid = -1;
  MemLogger *MemLogger::sInstance = nullptr;

  void MemLogger::dumpLog(std::ostream &out, const int &entriesPerThread) {
    std::priority_queue<TimestampedEntry, std::vector<TimestampedEntry>, std::greater<TimestampedEntry>> allEntries;

    std::cout << "Num logs: " << mNextThreadId - 1 << std::endl;
    for (std::vector<TimestampedEntry> &eachLog : mThreadLogs) {
      int start = ((entriesPerThread < 0) ? (0) : (eachLog.size() - entriesPerThread));
      if (start < 0) start = 0;
      std::cout << "Start: " << start << std::endl;
      std::cout << "Size: " << eachLog.size() << std::endl;
      for (int i = start; i < eachLog.size(); ++i) {
        allEntries.emplace(eachLog[i]);
      }
    }

    while (!allEntries.empty()) {
      const TimestampedEntry &each = allEntries.top();
      out << "{\n  tid = " << each.mTid << ",\n  time = " << each.mTimestamp
          << ",\n  entry = " << each.mEntry << "\n},\n";
      allEntries.pop();
    }
    out << std::flush;
  }

  void MemLogger::dumpLog(const char *const fileName, const int &entriesPerThread) {
    std::ofstream fOut(fileName);
    dumpLog(fOut, entriesPerThread);
  }

  void MemLogger::dumpLog(const int &entriesPerThread) {
    dumpLog(std::cout, entriesPerThread);
  }

  void MemLogger::rawDumpLog(std::ostream &out, const int &entriesPerThread) {
    std::priority_queue<TimestampedEntry, std::vector<TimestampedEntry>, std::greater<TimestampedEntry>> allEntries;

    for (std::vector<TimestampedEntry> &eachLog : mThreadLogs) {
      int start = ((entriesPerThread < 0) ? (0) : (eachLog.size() - entriesPerThread));
      if (start < 0) start = 0;
      for (int i = start; i < eachLog.size(); ++i) {
        allEntries.emplace(eachLog[i]);
      }
    }

    while (!allEntries.empty()) {
      const TimestampedEntry &each = allEntries.top();
      out << each.mEntry;
      allEntries.pop();
    }
    out << std::flush;
  }

  void MemLogger::rawDumpLog(const char *const fileName, const int &entriesPerThread) {
    std::ofstream fOut(fileName);
    rawDumpLog(fOut, entriesPerThread);
  }

  void MemLogger::rawDumpLog(const int &entriesPerThread) {
    rawDumpLog(std::cout, entriesPerThread);
  }

  void MemLogger::dumpYamlLog(std::ostream &out, const int &entriesPerThread) {
    std::priority_queue<TimestampedEntry, std::vector<TimestampedEntry>, std::greater<TimestampedEntry>> allEntries;

    for (std::vector<TimestampedEntry> &eachLog : mThreadLogs) {
      int start = ((entriesPerThread < 0) ? (0) : (eachLog.size() - entriesPerThread));
      if (start < 0) {
          start = 0;
      }
      for (int i = start; i < eachLog.size(); ++i) {
        allEntries.emplace(eachLog[i]);
      }
    }

    out << "---\n";
    while (!allEntries.empty()) {
      const TimestampedEntry &each = allEntries.top();
      out << "- tid: " << each.mTid
          << "\n  time: " << each.mTimestamp
          << "\n  entry: " << each.mEntry << "\n";
      allEntries.pop();
    }
    out << "...";
    out << std::flush;
  }

  void MemLogger::dumpYamlLog(const char *const fileName, const int &entriesPerThread) {
    std::ofstream fOut(fileName);
    dumpYamlLog(fOut, entriesPerThread);
  }

  void MemLogger::dumpYamlLog(const int &entriesPerThread) {
    dumpYamlLog(std::cout, entriesPerThread);
  }

  bool MemLogger::valid = true;
};

EXTERN_C void memlogger_logf(const char *const format, ...) {
    if (__builtin_expect(ks381::MemLogger::valid, 1)) {
        uint64_t entryTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    
        va_list vals, vals2;
        va_start(vals, format);
        va_copy(vals2, vals);
    
        size_t entryLength = vsnprintf(nullptr, 0, format, vals) + 1;
        std::string entryString;
        if (entryLength <= 0) {
          entryString = std::string("Bad vsnprintf!");
        } else {
          std::unique_ptr<char []> entryCString(new char[entryLength]);
          vsnprintf(entryCString.get(), entryLength, format, vals2);
          entryString = std::string(entryCString.get());
        }
    
        ks381::MemLogger::instance()->log(entryTime, entryString);
    }
}

EXTERN_C void memlogger_raw_dump_log() {
    ks381::MemLogger::instance()->rawDumpLog(-1);
}

EXTERN_C void memlogger_init() {
    ks381::MemLogger::init_instance();
}

EXTERN_C void memlogger_deinit() {
    ks381::MemLogger::deinit_instance();
}