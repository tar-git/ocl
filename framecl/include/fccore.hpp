//-----------------------------------------------------------------------------
//
// framecl core: context, device, etc
//
//-----------------------------------------------------------------------------

#pragma once

#include <cassert>
#include <fstream>
#include <sstream>
#include <vector>

// clang-format off
#include "cldefs.h"
#include "CL/cl2.hpp"
// clang-format on

#include "fcutils.hpp"
#include "fcplatform.hpp"

namespace framecl {

// output and input events
struct eventguards_t {
  cl::Event out;
  std::vector<cl::Event> ins;
};

// group of devices on single platform with shared context
// each have individual queue
template <typename Task> class devgroup_t final {
  cl::Context ctx_;
  std::vector<cl::Device> devices_;
  std::vector<cl::CommandQueue> queues_;
  int active_ = 0;

public:
  using task_t = Task;

  // this ctor means that program is file in opts.program
  devgroup_t(optparser_t opts) {
    assert(opts.parsed() && "You can not create devgroup before parsing opts");
    platform_list_t platforms;
    auto p = platforms.select(opts.platform());
    p.getDevices(CL_DEVICE_TYPE_ALL, &devices_);
    assert(devices_.size() > 0 && "Failed to query devices");
    ctx_ = cl::Context(devices_);
    queues_.resize(devices_.size());
    for (int i = 0, sz = queues_.size(); i < sz; ++i)
      queues_[i] = cl::CommandQueue(ctx_, devices_[i],
                                    cl::QueueProperties::Profiling |
                                        cl::QueueProperties::OutOfOrder);
  }

  int size() const { return devices_.size(); }

  // chooses to which device enqueue
  void select(int n) {
    assert(n >= 0 && n < size() &&
           "can not select device: number incorrect, check size()");
    active_ = n;
  }

  void enqueue(Task *pt, eventguards_t &evts) const {
    pt->enqueue_on(ctx_, queues_[active_], evts);
  }

  operator cl::Context() const { return ctx_; }
};

enum class task {
  write,
  process,
  read,
};

struct ITask {
  virtual void execute(cl::Context c, cl::CommandQueue q,
                       eventguards_t &evts) = 0;
  virtual ~ITask() {}
};

class task_t {
  task type_;
  std::unique_ptr<ITask> tsk_;

public:
  template <typename T, typename... Args>
  task_t(task type, T &obj, Args &&... args)
      : type_{type}, tsk_{obj.get_task(type, std::forward<Args>(args)...)} {}

  void enqueue_on(cl::Context c, cl::CommandQueue q,
                  eventguards_t &evts) const {
    assert(tsk_ && "Non-null task expected");
    tsk_->execute(c, q, evts);
  }

  void dump(std::ostream &os) const {
    os << "[task: " << tsk_.get() << " ";
    switch (type_) {
    case task::write:
      os << "write";
      break;
    case task::process:
      os << "process";
      break;
    case task::read:
      os << "read";
      break;
    default:
      throw std::runtime_error("unknown task type");
    }
    os << "]";
  }
};

using context_t = devgroup_t<task_t>;

class program_t final {
  context_t &ctx_;
  cl::Program p;

public:
  program_t(context_t &ctx, optparser_t opts) : ctx_{ctx} {
    assert(opts.parsed() && "You can not create devgroup before parsing opts");
    std::ifstream is;
    is.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    is.open(opts.program());
    std::stringstream os;
    os << is.rdbuf();

    p = cl::Program(ctx, os.str());
    try {
      p.build(); // "-cl-std=CL2.0" ?
    } catch (...) {
      auto buildInfo = p.getBuildInfo<CL_PROGRAM_BUILD_LOG>();
      std::cerr << "Error: build failed. Log:" << std::endl;
      for (auto &&pair : buildInfo)
        std::cerr << pair.second << std::endl << std::endl;
      throw;
    }
  }

  // UGLY breach of incapsulation
  context_t &context() { return ctx_; }

  operator cl::Program() const { return p; }
};

struct run_params_t {
  cl::NDRange offset, global, local;
};

template <typename... Ts> class functor_t final {
  context_t &ctx_;
  run_params_t rp_;
  cl::KernelFunctor<Ts...> kf_;

public:
  functor_t(program_t prog, run_params_t rp, std::string kname)
      : ctx_(prog.context()), rp_(rp), kf_(prog, kname) {}

  template <typename... Vs>
  std::unique_ptr<ITask> get_task(task type, Vs &&... args);
};

// simple buffer on host registered on device ctx
template <typename T> class buffer_t final {
  context_t &ctx_;
  int size_;
  std::vector<T> contents_;
  cl::Buffer buf_;

  // core buffer ctor and interface
public:
  buffer_t(context_t &ctx, int size)
      : ctx_(ctx), size_(size), contents_(size_),
        buf_(cl::Buffer(ctx, CL_MEM_READ_WRITE, size * sizeof(T))) {}

  buffer_t(const buffer_t &) = delete;

  std::unique_ptr<ITask> get_task(task type);

  // mimicing vector-like interface
public:
  auto begin() { return contents_.begin(); }
  auto end() { return contents_.end(); }
  auto begin() const { return contents_.begin(); }
  auto end() const { return contents_.end(); }
  auto rbegin() { return contents_.rbegin(); }
  auto rend() { return contents_.rend(); }
  auto rbegin() const { return contents_.rbegin(); }
  auto rend() const { return contents_.rend(); }

  int size() const noexcept { return contents_.size(); }
  T &operator[](int n) { return contents_[n]; }

  cl::Buffer base() const { return buf_; }

  void dump(std::ostream &os) { os << contents_; }
};

struct BufferReadTask : public ITask {
  int off_, size_;
  void *ptr_;
  cl::Buffer buf_;

  BufferReadTask(cl::Buffer buf, int off, int sz, void *ptr)
      : buf_(buf), off_(off), size_(sz), ptr_(ptr) {}

  void execute(cl::Context c, cl::CommandQueue q,
               eventguards_t &evts) override {
    q.enqueueReadBuffer(buf_, /* bloking */ false, off_, size_, ptr_, &evts.ins,
                        &evts.out);
  }
};

struct BufferWriteTask : public ITask {
  int off_, size_;
  void *ptr_;
  cl::Buffer buf_;

  BufferWriteTask(cl::Buffer buf, int off, int sz, void *ptr)
      : buf_(buf), off_(off), size_(sz), ptr_(ptr) {}

  void execute(cl::Context c, cl::CommandQueue q,
               eventguards_t &evts) override {
    q.enqueueWriteBuffer(buf_, /* bloking */ false, off_, size_, ptr_,
                         &evts.ins, &evts.out);
  }
};

template <typename F, typename... Args> struct ProgramExecTask : public ITask {
  run_params_t rp_;
  F func_;
  std::function<cl::Event(cl::EnqueueArgs)> f_;

  // not perfect forwarding because lambda will be used out of scope, so I don't
  // want to mess with dangling stuff
  // TODO: rethink it later
  ProgramExecTask(run_params_t rp, F func, Args... args)
      : rp_(rp), func_(func) {
    auto lam = [=, this](cl::EnqueueArgs eargs) mutable {
      return func_(eargs, std::forward<Args>(args)...);
    };
    f_ = lam;
  }

  void execute(cl::Context c, cl::CommandQueue q,
               eventguards_t &evts) override {
    cl::EnqueueArgs eargs(q, evts.ins, rp_.offset, rp_.global, rp_.local);
    evts.out = f_(eargs);
  }
};

template <typename T> std::unique_ptr<ITask> buffer_t<T>::get_task(task type) {
  if (type == task::read)
    return std::unique_ptr<ITask>{
        new BufferReadTask(buf_, 0, size_ * sizeof(T), contents_.data())};
  if (type == task::write)
    return std::unique_ptr<ITask>{
        new BufferWriteTask(buf_, 0, size_ * sizeof(T), contents_.data())};
  throw std::runtime_error("Illegal task for buffer");
}

template <typename... Ts>
template <typename... Vs>
std::unique_ptr<ITask> functor_t<Ts...>::get_task(task type, Vs &&... args) {
  if (type == task::process)
    return std::unique_ptr<ITask>{
        new ProgramExecTask(rp_, kf_, std::forward<Vs>(args)...)};
  throw std::runtime_error("Illegal task for program");
}

class depgraph_t {
  context_t &ctx_;
  std::vector<task_t *> tasks_;
  std::vector<cl::Event> evts_;
  std::unordered_map<task_t *, int> idx_;
  std::unordered_map<task_t *, std::vector<task_t *>> deps_;
  std::unordered_map<task_t *, int> task_levels_;
  std::vector<std::vector<task_t *>> level_tasks_;

public:
  depgraph_t(context_t &ctx,
             std::initializer_list<std::initializer_list<task_t *>> init)
      : ctx_(ctx) {
    // fill tasks, idx and deps
    level_tasks_.emplace_back();
    for (auto &&ls : init) {
      assert(ls.size() > 0 && "Void rows not acceptable");
      task_t *pt = *ls.begin();
      tasks_.push_back(pt);
      idx_[pt] = tasks_.size() - 1;

      // fill level 0 if no deps
      task_levels_[pt] = (ls.size() == 1) ? 0 : -1;
      if (ls.size() == 1)
        level_tasks_[0].push_back(pt);
      else {
        // fill deps
        for (auto &&it = std::next(ls.begin()), ite = ls.end(); it != ite; ++it)
          deps_[pt].push_back(*it);
      }
    }
    evts_.resize(tasks_.size());

    // fill levels: any pass of this loop will form next level
    for (;;) {
      bool all_set = true;
      bool levels_modified = false;
      for (auto &&pt : tasks_) {
        if (task_levels_[pt] != -1)
          continue;
        all_set = false;
        int level = -1;
        for (auto &&pdep : deps_[pt]) {
          int dep_lv = task_levels_[pdep];
          if (dep_lv == -1) {
            level = -1;
            break;
          }
          level = std::max(level, dep_lv);
        }
        if (level != -1) {
          levels_modified = true;
          task_levels_[pt] = level + 1;
          assert(level + 1 <= level_tasks_.size() &&
                 "This invariant shall hold by definition");
          if (level + 1 == level_tasks_.size())
            level_tasks_.emplace_back();
          level_tasks_[level + 1].push_back(pt);
        }
      }

      if (all_set)
        break;

      if (!levels_modified)
        throw std::runtime_error("incorrect dep-graph structure detected");
    }
  }

  void dump(std::ostream &os) const {
    int nlv = 0;
    for (auto &&lv : level_tasks_) {
      os << (nlv++) << ": ";
      for (auto &&tsk : lv) {
        tsk->dump(os);
        os << " ";
      }
      os << "\n";
    }
  }

  void execute() {
    std::vector<bool> last_active_;

    // peek task
    for (auto &&lv : level_tasks_) {
      std::vector<bool> active_(evts_.size(), false);
      for (auto &&pt : lv) {
        int id = idx_[pt];
        eventguards_t evt;
        for (auto &&pdep : deps_[pt]) {
          int dep_id = idx_[pdep];
#ifdef EVTDBG
          std::cout << "Dependency event: " << dep_id << "; ";
          int status =
              evts_[dep_id].getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>();
          std::cout << "Status: " << cstat(status) << "\n";
#endif
          evt.ins.push_back(evts_[dep_id]);
        }
        ctx_.enqueue(pt, evt);
        evts_[id] = evt.out;
#ifdef EVTDBG
        std::cout << "Produced event: " << id << "; ";
        int status = evts_[id].getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>();
        std::cout << "Status: " << cstat(status) << "\n";
#endif
        active_[id] = true;
      }
      last_active_.swap(active_);
    }

    // wait for last active events after all is about to finish
    for (int i = 0; i < last_active_.size(); ++i)
      if (last_active_[i]) {
        evts_[i].wait();
#ifdef EVTDBG
        std::cout << "Active event: " << i << "; ";
        int status = evts_[i].getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>();
        std::cout << "Status: " << cstat(status) << "\n";
#endif
      }
  }

  const char *cstat(int status) {
    switch (status) {
    case CL_QUEUED:
      return "Queued";
    case CL_SUBMITTED:
      return "Submitted";
    case CL_RUNNING:
      return "Running";
    case CL_COMPLETE:
      return "Complete";
    default:
      throw std::runtime_error("Unknown status of event");
    }
  }
};

}; // namespace framecl