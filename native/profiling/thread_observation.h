#pragma once
#include <cstdint>
#if defined(__APPLE__) && defined(__MACH__)
#include <cerrno>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/proc_info.h>
#include <sys/qos.h>
#include <time.h>
#include <unistd.h>
#endif

// Derived observation only. There are deliberately no scheduling/activity
// setters, task ports for other processes, allocations, saved authority or UI.
namespace coastal::profiling {
#if defined(__APPLE__) && defined(__MACH__)
struct ObservationErrnoRestore {int saved=errno;~ObservationErrnoRestore(){errno=saved;}};
#endif
inline constexpr std::uint64_t thread_identity_sample_period=30;
struct ThreadIdentity {
    bool supported=false;
    std::uint64_t thread_id=0;
    int thread_id_status=-1,qos_status=-1;
    std::uint32_t requested_qos=0,initial_main_qos=0;
    int relative_priority=0;
};
inline ThreadIdentity observe_current_thread_identity() noexcept {
    ThreadIdentity out;
#if defined(__APPLE__) && defined(__MACH__)
    ObservationErrnoRestore preserve_errno;
    out.supported=true;
    out.thread_id_status=pthread_threadid_np(nullptr,&out.thread_id);
    qos_class_t qos=QOS_CLASS_UNSPECIFIED;
    out.qos_status=pthread_get_qos_class_np(pthread_self(),&qos,&out.relative_priority);
    out.requested_qos=std::uint32_t(qos);
    out.initial_main_qos=std::uint32_t(qos_class_main());
#endif
    return out;
}
struct ThreadCpuSample {bool valid=false;std::uint64_t ns=0;};
inline ThreadCpuSample observe_current_thread_cpu() noexcept {
    ThreadCpuSample out;
#if defined(__APPLE__) && defined(__MACH__)
    ObservationErrnoRestore preserve_errno;
    timespec value{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)==0&&value.tv_sec>=0&&value.tv_nsec>=0){
        out.valid=true;out.ns=std::uint64_t(value.tv_sec)*1000000000ULL+std::uint64_t(value.tv_nsec);
    }
#endif
    return out;
}
struct ThreadMetric {
    std::uint64_t calls=0,wall_total_ns=0,wall_max_ns=0;
    std::uint64_t cpu_valid_calls=0,cpu_total_ns=0,cpu_max_ns=0,cpu_clock_failures=0;
    std::uint64_t identity_samples=0,requested_qos_changes=0,thread_id_changes=0;
    ThreadIdentity first_identity,last_identity;
};
inline void record_thread_identity(ThreadMetric& target,ThreadIdentity value) noexcept {
    if(target.identity_samples==0)target.first_identity=value;
    else{
        if(value.qos_status==0&&target.last_identity.qos_status==0
            &&(value.requested_qos!=target.last_identity.requested_qos
                ||value.relative_priority!=target.last_identity.relative_priority))++target.requested_qos_changes;
        if(value.thread_id_status==0&&target.last_identity.thread_id_status==0
            &&value.thread_id!=target.last_identity.thread_id)++target.thread_id_changes;
    }
    target.last_identity=value;++target.identity_samples;
}
inline void record_thread_duration(ThreadMetric& target,ThreadCpuSample begin,
                                   ThreadCpuSample end,std::uint64_t wall_ns) noexcept {
    ++target.calls;target.wall_total_ns+=wall_ns;
    if(wall_ns>target.wall_max_ns)target.wall_max_ns=wall_ns;
    if(!begin.valid||!end.valid||end.ns<begin.ns){++target.cpu_clock_failures;return;}
    const auto cpu=end.ns-begin.ns;++target.cpu_valid_calls;target.cpu_total_ns+=cpu;
    if(cpu>target.cpu_max_ns)target.cpu_max_ns=cpu;
}

// Joined-host diagnostic only. These priorities/run states describe the time
// of snapshot (workers will normally be waiting), not the entire measured work.
struct NativeThreadStatus {
    bool supported=false,valid=false;
    int error_number=0,policy=0,run_state=0,current_priority=0,base_priority=0,max_priority=0;
};
inline NativeThreadStatus observe_native_thread_status(std::uint64_t thread_id) noexcept {
    NativeThreadStatus out;
#if defined(__APPLE__) && defined(__MACH__)
    ObservationErrnoRestore preserve_errno;
    out.supported=true;if(thread_id==0){out.error_number=EINVAL;return out;}
    proc_threadinfo info{};
    const int size=proc_pidinfo(getpid(),PROC_PIDTHREADID64INFO,thread_id,&info,sizeof(info));
    if(size!=int(sizeof(info))){out.error_number=errno;return out;}
    out.valid=true;out.policy=info.pth_policy;out.run_state=info.pth_run_state;
    out.current_priority=info.pth_curpri;out.base_priority=info.pth_priority;out.max_priority=info.pth_maxpriority;
#else
    (void)thread_id;
#endif
    return out;
}
struct NativeProcessStatus {
    bool supported=false,flags_valid=false,role_valid=false;
    std::uint32_t raw_process_flags=0;
    bool suppressed_flag=false,darwin_background_flag=false,external_background_flag=false;
    int flags_error_number=0,nice_value=0,task_role=0,task_role_status=0;
};
inline NativeProcessStatus observe_native_process_status() noexcept {
    NativeProcessStatus out;
#if defined(__APPLE__) && defined(__MACH__)
    ObservationErrnoRestore preserve_errno;
    out.supported=true;proc_bsdinfo info{};
    if(proc_pidinfo(getpid(),PROC_PIDTBSDINFO,0,&info,sizeof(info))==sizeof(info)){
        out.flags_valid=true;out.raw_process_flags=info.pbi_flags;out.nice_value=info.pbi_nice;
        out.suppressed_flag=(info.pbi_flags&PROC_FLAG_SUPPRESSED)!=0;
        out.darwin_background_flag=(info.pbi_flags&PROC_FLAG_DARWINBG)!=0;
        out.external_background_flag=(info.pbi_flags&PROC_FLAG_EXT_DARWINBG)!=0;
    }else out.flags_error_number=errno;
    task_category_policy_data_t category{};mach_msg_type_number_t count=TASK_CATEGORY_POLICY_COUNT;
    boolean_t get_default=false;
    out.task_role_status=task_policy_get(mach_task_self(),TASK_CATEGORY_POLICY,
        reinterpret_cast<task_policy_t>(&category),&count,&get_default);
    if(out.task_role_status==KERN_SUCCESS&&count==TASK_CATEGORY_POLICY_COUNT){
        out.role_valid=true;out.task_role=int(category.role);
    }
#endif
    return out;
}
} // namespace coastal::profiling
