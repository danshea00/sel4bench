/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <autoconf.h>
#include <sel4benchirquser/gen_config.h>
#include <stdio.h>
#include <sel4runtime.h>
#include <muslcsys/vsyscall.h>
#include <utils/attribute.h>

#include <sel4platsupport/timer.h>
#include <sel4platsupport/irq.h>
#include <utils/time.h>

#include <platsupport/irq.h>
#include <platsupport/ltimer.h>

#include <sel4bench/arch/sel4bench.h>
#include <sel4bench/kernel_logging.h>
#include <sel4bench/logging.h>

#include <benchmark.h>
#include <irq.h>

#define INTERRUPT_PERIOD_NS (10 * NS_IN_MS)

void abort(void)
{
    benchmark_finished(EXIT_FAILURE);
}

void spinner_fn(int argc, char **argv)
{
    sel4bench_init();
    if (argc != 1) {
        abort();
    }

    volatile ccnt_t *current_time = (volatile ccnt_t *) atol(argv[0]);

    while (1) {
        /* just take the low bits so the reads are atomic */
        SEL4BENCH_READ_CCNT(*current_time);
    }
}

/* ep for ticker to Send on when done */
static seL4_CPtr done_ep;
/* ntfn for ticker to wait for timer irqs on */
static seL4_CPtr timer_signal;
/* initialised IRQ interface */
static ps_irq_ops_t *irq_ops;
/* ntfn_id of the timer notification provided to the IRQ interface */
static ntfn_id_t timer_ntfn_id;

void ticker_fn(ccnt_t *results, volatile ccnt_t *current_time)
{
    seL4_Word start, end_low;
    ccnt_t end;
    seL4_Word badge;

    for (int i = 0; i < N_RUNS; i++) {
        /* wait for irq */
        seL4_Wait(timer_signal, &badge);
        /* record result */
        SEL4BENCH_READ_CCNT(end);
        sel4platsupport_irq_handle(irq_ops, timer_ntfn_id, badge);
        end_low = (seL4_Word) end;
        start = (seL4_Word) * current_time;
        results[i] = end_low - start;
    }

    seL4_Send(done_ep, seL4_MessageInfo_new(0, 0, 0, 0));
}

void ticker_fn_ep(int argc, char **argv)
{
    if (argc != 5) {
        abort();
    }
    ccnt_t overhead = (ccnt_t) atol(argv[0]);
    ccnt_t *results_sum = (ccnt_t *) atol(argv[1]);
    ccnt_t *results_sum2 = (ccnt_t *) atol(argv[2]);
    ccnt_t *results_num = (ccnt_t *) atol(argv[3]);
    volatile ccnt_t *current_time = (volatile ccnt_t *) atol(argv[4]);

    seL4_Word start, end_low;
    ccnt_t end, sum = 0, sum2 = 0;
    seL4_Word badge;

    DATACOLLECT_INIT();

    for (seL4_Word i = 0; i < N_RUNS; i++) {
        /* wait for irq */
        seL4_Wait(timer_signal, &badge);
        /* record result */
        SEL4BENCH_READ_CCNT(end);
        sel4platsupport_irq_handle(irq_ops, timer_ntfn_id, badge);
        end_low = (seL4_Word) end;
        start = (seL4_Word) * current_time;
        DATACOLLECT_GET_SUMS(i, N_IGNORED, start, end_low, overhead, sum, sum2);
    }

    *results_sum = sum;
    *results_sum2 = sum2;
    *results_num = N_RUNS - N_IGNORED;

    seL4_Send(done_ep, seL4_MessageInfo_new(0, 0, 0, 0));
}

static env_t *env;

void high_prio_fn(int argc, char **argv)
{
    sel4bench_init();
    seL4_CPtr ntfn = (seL4_CPtr) atol(argv[0]);
    volatile bool *wait = (bool *) atol(argv[1]);
    ccnt_t *results = (ccnt_t *) atol(argv[2]);
    ccnt_t last, curr = 0;
    uint64_t i = 0;
    while (1) {
        last = curr;
        SEL4BENCH_READ_CCNT(curr);
        ccnt_t diff = curr - last;
        if (diff > 100 && !*wait) {
            results[i++] = diff;
            *wait = true;
            seL4_Wait(ntfn, NULL);
        } else {
            *wait = false;
        }
    }
}

void low_prio_fn(int argc, char **argv)
{
    seL4_CPtr ntfn = (seL4_CPtr) atol(argv[0]);
    volatile bool *wait = (bool *) atol(argv[1]);

    seL4_Word badge;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);

    for (int i = 0; i < N_RUNS; i++) {
        *wait ? seL4_NBSendWait(ntfn, tag, timer_signal, &badge) : seL4_Wait(timer_signal, &badge);
        sel4platsupport_irq_handle(irq_ops, timer_ntfn_id, badge);
    }
    seL4_Send(done_ep, seL4_MessageInfo_new(0, 0, 0, 0));
}

void CONSTRUCTOR(MUSLCSYS_WITH_VSYSCALL_PRIORITY) init_env(void)
{
    static size_t object_freq[seL4_ObjectTypeCount] = {
        [seL4_TCBObject] = 2,
        [seL4_EndpointObject] = 1,
#ifdef CONFIG_KERNEL_MCS
        [seL4_SchedContextObject] = 2,
        [seL4_ReplyObject] = 2,
        [seL4_NotificationObject] = 1,
#endif
    };

    env = benchmark_get_env(
              sel4runtime_argc(),
              sel4runtime_argv(),
              sizeof(irquser_results_t),
              object_freq
          );
}

int main(int argc, char **argv)
{
    irquser_results_t *results;
    vka_object_t endpoint = {0};

    benchmark_init_timer(env);
    results = (irquser_results_t *) env->results;

    if (vka_alloc_endpoint(&env->slab_vka, &endpoint) != 0) {
        ZF_LOGF("Failed to allocate endpoint\n");
    }

    /* set up globals */
    done_ep = endpoint.cptr;
    timer_signal = env->ntfn.cptr;
    irq_ops = &env->io_ops.irq_ops;
    timer_ntfn_id = env->ntfn_id;

    int error = ltimer_reset(&env->ltimer);
    ZF_LOGF_IF(error, "Failed to start timer");

    error = ltimer_set_timeout(&env->ltimer, INTERRUPT_PERIOD_NS, TIMEOUT_PERIODIC);
    ZF_LOGF_IF(error, "Failed to configure timer");

    sel4bench_init();

    sel4utils_thread_t ticker, spinner, low;

    /* measurement overhead */
    ccnt_t start, end;
    for (int i = 0; i < N_RUNS; i++) {
        SEL4BENCH_READ_CCNT(start);
        SEL4BENCH_READ_CCNT(end);
        results->overheads[i] = end - start;
    }

    /* find the minimum overhead for early processing run */
    results->overhead_min = getMinOverhead(results->overheads, N_RUNS);

    /* create a frame for the shared time variable so we can share it between processes */
    ccnt_t *local_current_time = (ccnt_t *) vspace_new_pages(&env->vspace, seL4_AllRights, 1, seL4_PageBits);
    if (local_current_time == NULL) {
        ZF_LOGF("Failed to allocate page");
    }

    /* first run the benchmark between two threads in the current address space */
    benchmark_configure_thread(env, endpoint.cptr, seL4_MaxPrio - 1, "ticker", &ticker);
    benchmark_configure_thread(env, endpoint.cptr, seL4_MaxPrio - 2, "spinner", &spinner);

    error = sel4utils_start_thread(&ticker, (sel4utils_thread_entry_fn) ticker_fn, (void *) results->thread_results,
                                   (void *) local_current_time, true);
    if (error) {
        ZF_LOGF("Failed to start ticker");
    }

    char strings[1][WORD_STRING_SIZE];
    char *spinner_argv[1];

    sel4utils_create_word_args(strings, spinner_argv, 1, (seL4_Word) local_current_time);
    error = sel4utils_start_thread(&spinner, (sel4utils_thread_entry_fn) spinner_fn, (void *) 1, (void *) spinner_argv,
                                   true);
    assert(!error);

    benchmark_wait_children(endpoint.cptr, "child of irq-user", 1);

    /* stop spinner thread */
    error = seL4_TCB_Suspend(spinner.tcb.cptr);
    assert(error == seL4_NoError);

    error = seL4_TCB_Suspend(ticker.tcb.cptr);
    assert(error == seL4_NoError);

    /* run the benchmark again with early processing */
    char ticker_ep_strings[5][WORD_STRING_SIZE];
    char *ticker_ep_argv[5];
    sel4utils_create_word_args(ticker_ep_strings, ticker_ep_argv, 5, (seL4_Word) results->overhead_min,
                               &results->thread_results_ep_sum,
                               &results->thread_results_ep_sum2, &results->thread_results_ep_num, (seL4_Word) local_current_time);
    error = sel4utils_start_thread(&ticker, (sel4utils_thread_entry_fn) ticker_fn_ep, (void *) 5, (void *) ticker_ep_argv,
                                   true);
    if (error) {
        ZF_LOGF("Failed to start ticker");
    }

    error = sel4utils_start_thread(&spinner, (sel4utils_thread_entry_fn) spinner_fn, (void *) 1, (void *) spinner_argv,
                                   true);
    assert(!error);

    benchmark_wait_children(endpoint.cptr, "child of irq-user", 1);

    /* stop spinner thread */
    error = seL4_TCB_Suspend(spinner.tcb.cptr);
    assert(error == seL4_NoError);

    error = seL4_TCB_Suspend(ticker.tcb.cptr);
    assert(error == seL4_NoError);

    /* now run the benchmark again, but run the spinner in another address space */

    /* restart ticker */
    error = sel4utils_start_thread(&ticker, (sel4utils_thread_entry_fn) ticker_fn, (void *) results->process_results,
                                   (void *) local_current_time, true);
    assert(!error);

    sel4utils_process_t spinner_process;
    benchmark_shallow_clone_process(env, &spinner_process, seL4_MaxPrio - 2, spinner_fn, "spinner");

    /* share the current time variable with the spinner process */
    void *current_time_remote = vspace_share_mem(&env->vspace, &spinner_process.vspace,
                                                 (void *) local_current_time, 1, seL4_PageBits,
                                                 seL4_AllRights, true);
    assert(current_time_remote != NULL);

    /* start the spinner process */
    sel4utils_create_word_args(strings, spinner_argv, 1, (seL4_Word) current_time_remote);
    error = benchmark_spawn_process(&spinner_process, &env->slab_vka, &env->vspace, 1, spinner_argv, 1);
    if (error) {
        ZF_LOGF("Failed to start spinner process");
    }

    benchmark_wait_children(endpoint.cptr, "child of irq-user", 1);

    /* stop threads */
    error = seL4_TCB_Suspend(spinner_process.thread.tcb.cptr);
    assert(error == seL4_NoError);

    error = seL4_TCB_Suspend(ticker.tcb.cptr);
    assert(error == seL4_NoError);

    /* run the benchmark again but with early processing */
    sel4utils_create_word_args(ticker_ep_strings, ticker_ep_argv, 5, (seL4_Word) results->overhead_min,
                               &results->process_results_ep_sum,
                               &results->process_results_ep_sum2, &results->process_results_ep_num, (seL4_Word) local_current_time);
    error = sel4utils_start_thread(&ticker, (sel4utils_thread_entry_fn) ticker_fn_ep, (void *) 5, (void *) ticker_ep_argv,
                                   true);
    assert(!error);

    /* start the spinner process */
    sel4utils_create_word_args(strings, spinner_argv, 1, (seL4_Word) current_time_remote);
    error = benchmark_spawn_process(&spinner_process, &env->slab_vka, &env->vspace, 1, spinner_argv, 1);
    if (error) {
        ZF_LOGF("Failed to start spinner process");
    }

    benchmark_wait_children(endpoint.cptr, "child of irq-user", 1);

    /* stop threads */
    error = seL4_TCB_Suspend(spinner_process.thread.tcb.cptr);
    assert(error == seL4_NoError);

    error = seL4_TCB_Suspend(ticker.tcb.cptr);
    assert(error == seL4_NoError);

    // init kernel log
    // vka_object_t kernel_log_frame;
    // if (vka_alloc_frame(&env->slab_vka, seL4_LargePageBits, &kernel_log_frame) != 0) {
    //     ZF_LOGF("Failed to allocate ipc buffer");
    // }

    // if (kernel_logging_set_log_buffer(kernel_log_frame.cptr) != 0) {
    //     ZF_LOGF("Failed to set kernel log buffer");
    // }

    // void *log_buffer = vspace_map_pages(&env->vspace, &kernel_log_frame.cptr, NULL, seL4_AllRights, 1, seL4_LargePageBits, 1);
    // kernel_log_entry_t *kernel_log_buffer = (kernel_log_entry_t *)log_buffer;

    // set this threads priority to be the lowest
    seL4_CPtr auth = simple_get_tcb(&env->simple);
    seL4_TCB_SetPriority(SEL4UTILS_TCB_SLOT, auth, seL4_MaxPrio - 3);

    // local variables
    vka_object_t ntfn = {0};
    error = vka_alloc_notification(&env->slab_vka, &ntfn);
    ZF_LOGF_IF(error, "Failed to allocate notification object");

    seL4_CPtr sched_control = simple_get_sched_ctrl(&env->simple, 0);
    if (sched_control == seL4_CapNull) {
        ZF_LOGF("Failed to get sched control cap");
    }

    benchmark_configure_thread(env, endpoint.cptr, seL4_MaxPrio - 2, "low_prio", &low);
    error = seL4_SchedControl_Configure(sched_control, low.sched_context.cptr, 5 * US_IN_S, 5 * US_IN_S, 0, 0);
    ZF_LOGF_IF(error != seL4_NoError, "Failed to configure schedcontext");

    sel4utils_process_t high_process;
    benchmark_shallow_clone_process(env, &high_process, seL4_MaxPrio - 1, high_prio_fn, "high_prio");
    error = seL4_SchedControl_Configure(sched_control, high_process.thread.sched_context.cptr, 5 * US_IN_S, 5 * US_IN_S, 0, 0);
    ZF_LOGF_IF(error != seL4_NoError, "Failed to configure schedcontext");

    bool *wait = (bool *) vspace_new_pages(&env->vspace, seL4_AllRights, 1, seL4_PageBits);
    assert(wait != NULL);
    *wait = false;

    void *wait_remote = vspace_share_mem(&env->vspace, &high_process.vspace, wait, 1, seL4_PageBits, seL4_AllRights, true);
    assert(wait_remote != NULL);

    ccnt_t *results_local = (ccnt_t *) vspace_new_pages(&env->vspace, seL4_AllRights, 1, seL4_PageBits);

    void *results_remote = vspace_share_mem(&env->vspace, &high_process.vspace, results_local, 1, seL4_PageBits, seL4_AllRights, true);
    assert(results_remote != NULL);
    
    cspacepath_t ntfn_path;

    vka_cspace_make_path(&env->slab_vka, ntfn.cptr, &ntfn_path);
    seL4_CPtr ntfn_remote = sel4utils_copy_path_to_process(&high_process, ntfn_path);
    assert(ntfn_remote != seL4_CapNull);

    char low_prio_strings[2][WORD_STRING_SIZE];
    char *low_prio_argv[2];
    sel4utils_create_word_args(low_prio_strings, low_prio_argv, 2, ntfn.cptr, wait);

    char high_prio_strings[3][WORD_STRING_SIZE];
    char *high_prio_argv[3];
    sel4utils_create_word_args(high_prio_strings, high_prio_argv, 3, ntfn_remote, wait_remote, results_remote);


    error = sel4utils_start_thread(&low, (sel4utils_thread_entry_fn) low_prio_fn, (void *) 2, low_prio_argv, true);
    if (error) {
        ZF_LOGF("Failed to start low prio thread");
    }

    error = benchmark_spawn_process(&high_process, &env->slab_vka, &env->vspace, 3, high_prio_argv, 1);
    if (error) {
        ZF_LOGF("Failed to start test process");
    }

    benchmark_wait_children(endpoint.cptr, "children of irq-user", 1);

    // seL4_Word cnt = kernel_logging_finalize_log();
    // printf("cnt: %d\n", cnt);

    // for (int i = 0; i < cnt; i++) {

    //     seL4_Word id = kernel_logging_entry_get_key(&kernel_log_buffer[i]);
    //     seL4_Word value = kernel_logging_entry_get_data(&kernel_log_buffer[i]);
    //     results->tracepoints[id][i] = value;
    // }

    // copy results_local to results
    memcpy(results->irq_signal_low_results, results_local, N_RUNS * sizeof(ccnt_t));

    /* done -> results are stored in shared memory so we can now return */
    benchmark_finished(EXIT_SUCCESS);
    return 0;
}
