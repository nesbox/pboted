/**
 * Copyright (C) 2013-2016, The PurpleI2P Project
 * Copyright (C) 2019-2022 polistern
 *
 * This file is part of pboted project and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#ifndef PBOTED_SRC_QUEUE_H__
#define PBOTED_SRC_QUEUE_H__

#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace pbote {
namespace util {

template<typename Element>
class Queue {
 public:
  void Put(Element e) {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    m_Queue.push(std::move(e));
    m_NonEmpty.notify_one();
  }

  template<template<typename, typename...> class Container, typename... R>
  void Put(const Container<Element, R...> &vec) {
    if (!vec.empty()) {
      std::unique_lock<std::mutex> l(m_QueueMutex);
      for (const auto &it : vec)
        m_Queue.push(std::move(it));
      m_NonEmpty.notify_one();
    }
  }

  Element GetNext() {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    auto el = GetNonThreadSafe();
    if (!el) {
      m_NonEmpty.wait(l);
      el = GetNonThreadSafe();
    }
    return el;
  }

  Element GetNextWithTimeout(int usec) {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    auto el = GetNonThreadSafe();
    if (!el) {
      m_NonEmpty.wait_for(l, std::chrono::milliseconds(usec));
      el = GetNonThreadSafe();
    }
    return el;
  }

  void Wait() {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    m_NonEmpty.wait(l);
  }

  bool Wait(int sec, int usec) {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    return m_NonEmpty.wait_for(l, std::chrono::seconds(sec) +
        std::chrono::milliseconds(usec)) !=
        std::cv_status::timeout;
  }

  bool IsEmpty() {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    return m_Queue.empty();
  }

  int GetSize() {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    return m_Queue.size();
  }

  void WakeUp() { m_NonEmpty.notify_all(); };

  Element Get() {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    return GetNonThreadSafe();
  }

  Element Peek() {
    std::unique_lock<std::mutex> l(m_QueueMutex);
    return GetNonThreadSafe(true);
  }

 private:
  Element GetNonThreadSafe(bool peek = false) {
    if (!m_Queue.empty()) {
      auto el = m_Queue.front();
      if (!peek)
        m_Queue.pop();
      return el;
    }
    return nullptr;
  }

 private:
  std::queue<Element> m_Queue;
  std::mutex m_QueueMutex;
  std::condition_variable m_NonEmpty;
};

} // namespace util
} // namespace pbote

#endif // PBOTED_SRC_QUEUE_H__
