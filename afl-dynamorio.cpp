#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <string.h>
#include "droption.h"
#include "dr_api.h"
#include "drwrap.h"
#include "drmgr.h"
#include "drreg.h"
#include "drx.h"
#include "drsyms.h"
#include "afl/config.h"
#include <iostream>

static droption_t < bool > alt(DROPTION_SCOPE_CLIENT, "alternative", false, "alternative intstrumenting, 1% slower but better", "alternative instrumenting that reports fewer blocks, but 95% of the important ones, so fewer collisions. 1% less speed though.");
static droption_t < bool > libs(DROPTION_SCOPE_CLIENT, "libs", false, "also trace dynamic libraries", "afl-dynamorio by default only reports basic blocks of the main program, not libraries.");
static droption_t < std::string > exitpoint(DROPTION_SCOPE_CLIENT, "exitpoint", "", "exit when this address or function is hit", "exitpoint to patch exit() into. function name or 0xaddr is OK as well");
static droption_t < bool > forkserver(DROPTION_SCOPE_CLIENT, "forkserver", false, "install a fork server into the binary into main", "install a fork server into the program, if not configured with -entrypoint it will be in main()");
static droption_t < std::string > entrypoint(DROPTION_SCOPE_CLIENT, "entrypoint", "main", "entrypoint to patch the fork server into, defaults to main. 0xaddr is OK as well", "entrypoint to patch the fork server into, defaults to main. 0xaddr is OK as well");

#if MAP_SIZE_POW2 == 16
typedef uint16_t map_t;
#else
typedef uint32_t map_t;
#endif

static bool report_next_inst = false;
static generic_func_t addr_ep = 0, addr_fs = 0, addr_exit, addr_replace;
static reg_t xsp_ep;
static map_t prev_id;
static thread_id_t main_thread = -1;
static int tcls_idx = -1, now_start = 0, loadedlibs = 0, have_forkserver = 0;
#ifndef DEBUG
static uint8_t *trace_bits = NULL;
#endif
#ifdef TESTER
static FILE *test;
#endif


static void start_forkserver() {
  uint8_t tmp[4];
  int32_t child_pid = 0;
#ifdef DEBUG
  fprintf(stderr, "forkserver(): start\n");
#endif
  if (write(FORKSRV_FD + 1, tmp, 4) != 4) {
    dr_printf("Error writing fork server\n");
    dr_abort();
  }

  while (1) {
    uint32_t was_killed;
    int32_t status;

#ifdef DEBUG
    fprintf(stderr, "forkserver(): waiting for mother\n");
#endif
    if (read(FORKSRV_FD, &was_killed, 4) != 4) {
      dr_printf("Error reading fork server\n");
      dr_abort();
    }
    child_pid = fork();
    if (child_pid < 0) {
      dr_printf("Error fork\n");
      dr_abort();
    }
    if (child_pid == 0) {       // child
#ifdef DEBUG
      fprintf(stderr, "forkserver(): this is the child\n");
#endif
      close(FORKSRV_FD);
      close(FORKSRV_FD + 1);
      now_start = 1;
      return;
    }
    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) {
      dr_printf("Error writing fork server (2)\n");
      dr_abort();
    }
#ifdef DEBUG
    fprintf(stderr, "forkserver(): this is the forkserver(main)\n");
#endif
    if (waitpid(child_pid, &status, 0) < 0) {
      dr_printf("Error waiting for child\n");
      dr_abort();
    }
    if (write(FORKSRV_FD + 1, &status, 4) != 4) {
      dr_printf("Fork server is gone, terminating\n");
      dr_abort();
    }
#ifdef DEBUG
    fprintf(stderr, "forkserver(): child is done\n");
#endif
  }
}

static dr_emit_flags_t event_app_instruction(void *drcontext, void *tag, instrlist_t * bb, instr_t * inst, bool for_trace, bool translating, void *user_data) {
  /* By default drmgr enables auto-predication, which predicates all instructions with
   * the predicate of the current instruction on ARM.
   * We disable it here because we want to unconditionally execute the following
   * instrumentation. */
  drmgr_disable_auto_predication(drcontext, bb);
  if (!drmgr_is_first_instr(drcontext, inst))
    return DR_EMIT_DEFAULT;
#ifdef DEBUG
  dr_printf("Inst of BB at " PFX " of thread id %d at start %d: isindirect:%d iscond:%d mbr:%d isret:%d\n", tag, dr_get_thread_id(drcontext), now_start,
            instr_is_call_indirect(inst), instr_is_cbr(inst), instr_is_mbr(inst), instr_is_return(inst));
  instr_disassemble(drcontext, inst, STDOUT);
  dr_printf("\n");
#endif
  if (now_start == 0) {
    if (tag == addr_ep && have_forkserver == 0) {
      have_forkserver = 1;
      start_forkserver();
    }
    return DR_EMIT_DEFAULT;
  } else {
    map_t id = (map_t) (((uintptr_t) tag) >> 1);
#ifdef DEBUG
    dr_printf("BB " PFX " trace %04x of %04x ^ %04x\n", tag, prev_id ^ id, prev_id, id);
#else
#ifdef TESTER
    fprintf(test, "BB " PFX " trace %04x of %04x ^ %04x\n", tag, prev_id ^ id, prev_id, id);
#endif
    trace_bits[prev_id ^ id]++;
#endif
    prev_id = id >> 1;
    return DR_EMIT_DEFAULT;
  }
}

static dr_emit_flags_t event_app_instruction_alt(void *drcontext, void *tag, instrlist_t * bb, instr_t * inst, bool for_trace, bool translating, void *user_data) {
  if (now_start == 0) {
    if (tag == addr_ep && have_forkserver == 0) {
      have_forkserver = 1;
      start_forkserver();
    }
    return DR_EMIT_DEFAULT;
  } else {
    drmgr_disable_auto_predication(drcontext, bb);
    if (report_next_inst == true) {
      map_t id = (map_t) (((uintptr_t) tag) >> 1);
#ifdef DEBUG
      dr_printf("Inst of AFTER at " PFX " of thread id %d at start %d: iscallindirect:%d iscond:%d mbr:%d isret:%d isubr:%d\n", tag, dr_get_thread_id(drcontext), now_start,
                instr_is_call_indirect(inst), instr_is_cbr(inst), instr_is_mbr(inst), instr_is_return(inst), instr_is_ubr(inst));
      instr_disassemble(drcontext, inst, STDOUT);
      dr_printf("\n");
      dr_printf("BB " PFX " trace %04x of %04x ^ %04x\n", tag, prev_id ^ id, prev_id, id);
#else
#ifdef TESTER
      fprintf(test, "BB " PFX " trace %04x of %04x ^ %04x\n", tag, prev_id ^ id, prev_id, id);
#endif
      trace_bits[prev_id ^ id]++;
#endif
      prev_id = id >> 1;
      report_next_inst = false;
    }
    if (!drmgr_is_last_instr(drcontext, inst))
      return DR_EMIT_DEFAULT;
    if (instr_is_return(inst) || (!instr_is_cbr(inst) && !instr_is_call_indirect(inst) && !instr_is_mbr(inst)))
      return DR_EMIT_DEFAULT;

    report_next_inst = true;
#ifdef DEBUG
    dr_printf("Inst of BEFORE at " PFX " of thread id %d at start %d: iscallindirect:%d iscond:%d mbr:%d isret:%d isubr:%d\n", tag, dr_get_thread_id(drcontext), now_start,
              instr_is_call_indirect(inst), instr_is_cbr(inst), instr_is_mbr(inst), instr_is_return(inst), instr_is_ubr(inst));
    instr_disassemble(drcontext, inst, STDOUT);
    dr_printf("\n");
#endif
    return DR_EMIT_DEFAULT;
  }
}

static void event_fork(void *drcontext) {
#ifdef TESTER
  fprintf(test, "fork event, new child is thread %d\n", dr_get_thread_id(drcontext));
#endif
  now_start = 1;
  prev_id = 0;
}

static void module_load_event(void *drcontext, const module_data_t * mod, bool loaded) {
  if (loadedlibs == 0) {
    if (forkserver.get_value() == true && addr_ep == 0) {
      addr_ep = dr_get_proc_address(mod->handle, entrypoint.get_value().c_str());
      if (!addr_ep) {
        drsym_init(0);
        drsym_lookup_symbol(mod->full_path, entrypoint.get_value().c_str(), (size_t *) (&addr_ep), 0);
        drsym_exit();
      }
      DR_ASSERT_MSG(addr_ep, "Can not find entrypoint");
      dr_printf("ep: %p\n", addr_ep);
      uintptr_t a = (uintptr_t) addr_ep;
      uintptr_t b = (uintptr_t) mod->start;
      uintptr_t c = a + b;
      addr_ep = (generic_func_t) c;
#ifdef DEBUG
      dr_printf("ep: %p\n", addr_ep);
      dr_printf("start: %p  entry: %p  end: %p\n", mod->start, mod->entry_point, mod->end);
#endif
    }
    if (exitpoint.get_value().length() > 0 && addr_exit == 0) {
      addr_exit = dr_get_proc_address(mod->handle, exitpoint.get_value().c_str());
      if (!addr_exit) {
        drsym_init(0);
        drsym_lookup_symbol(mod->full_path, exitpoint.get_value().c_str(), (size_t *) (&addr_exit), 0);
        drsym_exit();
      }
      if (!addr_exit) {
        if (libs.get_value() == false)
          DR_ASSERT_MSG(false, "Can not find exitpoint");
      } else {
        uintptr_t a = (uintptr_t) addr_exit;
        uintptr_t b = (uintptr_t) mod->start;
        uintptr_t c = a + b;
        addr_exit = (generic_func_t) c;
      }
    }
  }
  if (strstr(dr_module_preferred_name(mod), "libdynamorio") != NULL || strstr(dr_module_preferred_name(mod), "libafl-dynamorio") != NULL) {
    dr_module_set_should_instrument(mod->handle, false);
    return;
  }
  if (libs.get_value() == false) {
    if (loadedlibs > 0) {
      dr_module_set_should_instrument(mod->handle, false);
#ifdef DEBUG
      dr_printf("disabled instrumentation for library %s\n", dr_module_preferred_name(mod));
#else
#ifdef TESTER
      fprintf(test, "disabled instrumentation for library %s\n", dr_module_preferred_name(mod));
#endif
#endif
    }
#ifdef DEBUG
    else
      dr_printf("performing instrumentation for library %s\n", dr_module_preferred_name(mod));
#else
#ifdef TESTER
    else
      fprintf(test, "performing instrumentation for library %s\n", dr_module_preferred_name(mod));
#endif
#endif
  }
  if (loadedlibs > 0) {
    // something still to lookup?
    if (exitpoint.get_value().length() > 0 && addr_exit == 0) {
      addr_exit = dr_get_proc_address(mod->handle, exitpoint.get_value().c_str());
      if (!addr_exit) {
        drsym_init(0);
        drsym_lookup_symbol(mod->full_path, exitpoint.get_value().c_str(), (size_t *) (&addr_exit), 0);
        drsym_exit();
      }
    }
    // XXX BUG TODO
    // now try to replace
    if (addr_exit) {
      app_pc loc_exit = (app_pc) dr_get_proc_address(mod->handle, "_exit");
      if (loc_exit) {
#ifdef DEBUG
        dr_printf("found _exit in in %s at %p\n", dr_module_preferred_name(mod), loc_exit);
#endif
        drwrap_replace_native((app_pc) addr_exit, (app_pc) loc_exit, true, 0, NULL, true);
        addr_exit = 0;          // we take the first
      }
    }
  }
  loadedlibs++;
}

static void event_thread_context_init(void *drcontext, bool new_depth) {
#ifdef DEBUG
  dr_printf("new thread: %d (%d)\n", dr_get_thread_id(drcontext), new_depth);
#endif
  if (main_thread == 0)
    main_thread = dr_get_thread_id(drcontext);
}

static void event_thread_context_exit(void *drcontext, bool thread_exit) {
#ifdef DEBUG
  dr_printf("exit thread: %d (%d)\n", dr_get_thread_id(drcontext), thread_exit);
#endif
}

static void event_exit(void) {
#ifdef TESTER
  fclose(test);
#endif
  drmgr_unregister_cls_field(event_thread_context_init, event_thread_context_exit, tcls_idx);
  drx_exit();
  drwrap_exit();
  drmgr_exit();
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
#ifndef DEBUG
  char *shmenv;
  int32_t shm_id = -1;
#endif
  std::string parse_err;
  dr_set_client_name("Running DynamoRIO afl-dynamorio", "https://mh-sec.de/");
  if (droption_parser_t::parse_argv(DROPTION_SCOPE_CLIENT, argc, argv, &parse_err, NULL) != 1) {
    std::cout << "afl-dynamorio (c) 2018-2019 by Marc \"van Hauser\" Heuse <mh@mh-sec.de> AGPL 3.0" << std::endl;
    std::cout << "===========================================================================" << std::endl;
    std::cout << droption_parser_t::usage_long(DROPTION_SCOPE_ALL, "", "", " (", ")", "  ", "\n");
    dr_abort();
  }
  if (!drmgr_init() || !drx_init() || !drwrap_init())
    DR_ASSERT(false);

  disassemble_set_syntax(DR_DISASM_INTEL);

#ifndef DEBUG
  if ((shmenv = getenv(SHM_ENV_VAR)) == NULL)
    DR_ASSERT_MSG(false, "AFL environment variable " SHM_ENV_VAR " not set");
  if ((shm_id = atoi(shmenv)) < 0)
    DR_ASSERT_MSG(false, "invalid " SHM_ENV_VAR " contents");
  if ((trace_bits = (u8 *) shmat(shm_id, NULL, 0)) == (void *) -1 || trace_bits == NULL)
    DR_ASSERT_MSG(false, SHM_ENV_VAR " attach failed");
  if (fcntl(FORKSRV_FD, F_GETFL) == -1 || fcntl(FORKSRV_FD + 1, F_GETFL) == -1)
    DR_ASSERT_MSG(false, "AFL fork server file descriptors are not open");
#endif

  if (strncmp(entrypoint.get_value().c_str(), "0x", 2) == 0) {
    uint64_t tmp = strtoul(entrypoint.get_value().c_str(), NULL, 0);
    addr_ep = (generic_func_t) tmp;
  }
  if (strncmp(exitpoint.get_value().c_str(), "0x", 2) == 0) {
    uint64_t tmp = strtoul(exitpoint.get_value().c_str(), NULL, 0);
    addr_exit = (generic_func_t) tmp;
  }

  dr_register_exit_event(event_exit);
  drmgr_register_module_load_event(module_load_event);
  if (alt.get_value() == true) {
    if (!drmgr_register_bb_instrumentation_event(NULL, event_app_instruction_alt, NULL))
      DR_ASSERT(false);
  } else {
    if (!drmgr_register_bb_instrumentation_event(NULL, event_app_instruction, NULL))
      DR_ASSERT(false);
  }
  dr_register_fork_init_event(event_fork);

#ifdef TESTER
  if ((test = fopen("debug.log", "w+")) == NULL)
    DR_ASSERT_MSG(false, "can not create debug.log");
#endif

#ifdef DEBUG
  dr_printf("DynamoRIO client afl-dynamorio running\n");
#warning "compiled with debug"
  now_start = 1;
#endif
}
