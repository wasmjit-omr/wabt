/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "syscall.hpp"
#include "src/binary-reader-interp.h"
#include "src/binary-reader.h"
#include "src/cast.h"
#include "src/error-handler.h"
#include "src/feature.h"
#include "src/interp.h"
#include "src/literal.h"
#include "src/option-parser.h"
#include "src/resolve-names.h"
#include "src/stream.h"
#include "src/validator.h"
#include "src/wast-lexer.h"
#include "src/wast-parser.h"

#include "Jit.hpp"

using namespace wabt;
using namespace wabt::interp;

static int s_verbose;
static const char* s_infile;
static Thread::Options s_thread_options;
static Stream* s_trace_stream;
static bool s_disable_jit;
static bool s_trap_on_failed_comp;
static uint32_t s_jit_threshold = 1;
static Features s_features;

static std::unique_ptr<FileStream> s_log_stream;
static std::unique_ptr<FileStream> s_stdout_stream;

enum class RunVerbosity {
  Quiet = 0,
  Verbose = 1,
};

static const char s_description[] =
    R"(  read a file in the wasm binary format, and run in it a stack-based
  interpreter with support for musl libc.

examples:
  # parse binary file test.wasm, and run it
  $ wasm-interp test.wasm

  # parse test.wasm, run it and trace the output
  $ wasm-interp test.wasm --trace

  # parse test.wasm and run it, setting the value stack size to 100 elements
  $ wasm-interp test.wasm -V 100
)";

static void ParseOptions(int argc, char** argv) {
  OptionParser parser("libc-interp", s_description);

  parser.AddOption('v', "verbose", "Use multiple times for more info", []() {
    s_verbose++;
    s_log_stream = FileStream::CreateStdout();
  });
  parser.AddHelpOption();
  s_features.AddOptions(&parser);
  parser.AddOption('V', "value-stack-size", "SIZE",
                   "Size in elements of the value stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.value_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('C', "call-stack-size", "SIZE",
                   "Size in elements of the call stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.call_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('t', "trace", "Trace execution",
                   []() { s_trace_stream = s_stdout_stream.get(); });
  parser.AddOption("disable-jit",
                   "Prevent just in time compilation",
                   []() { s_disable_jit = true; });
  parser.AddOption("trap-on-failed-comp",
                   "Trap if a JIT compilation fails",
                   []() { s_trap_on_failed_comp = true; });
  parser.AddOption('\0', "jit-threshold", "THRESHOLD",
                   "Number of calls after which to JIT compile a function",
                   [](const std::string& argument) {
                     // TODO(thomasbc): validate
                     s_jit_threshold = atoi(argument.c_str());
                   });

  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) { s_infile = argument; });
  parser.Parse(argc, argv);
}

static void RunStart(interp::Module* module,
                     Environment* env,
                     Executor* executor,
                     RunVerbosity verbose) {
  TypedValues args;
  TypedValues results;
  interp::Export* e = module->GetExport("_start");

  ExecResult exec_result = executor->RunExport(e, args);
  if (verbose == RunVerbosity::Verbose) {
    WriteCall(s_stdout_stream.get(), string_view(), e->name, args,
              exec_result.values, exec_result.result);
    if (exec_result.result != interp::Result::Ok) {
      exec_result.PrintCallStack(s_stdout_stream.get(), env);
    }
  }
}

static wabt::Result ReadModule(const char* module_filename,
                               Environment* env,
                               ErrorHandler* error_handler,
                               DefinedModule** out_module) {
  wabt::Result result;
  std::vector<uint8_t> file_data;

  *out_module = nullptr;

  result = ReadFile(module_filename, &file_data);
  if (Succeeded(result)) {
    const bool kReadDebugNames = true;
    const bool kStopOnFirstError = true;
    ReadBinaryOptions options(s_features, s_log_stream.get(), kReadDebugNames,
                              kStopOnFirstError);
    result = ReadBinaryInterp(env, DataOrNull(file_data), file_data.size(),
                              &options, error_handler, out_module);

    if (Succeeded(result)) {
      if (s_verbose)
        env->DisassembleModule(s_stdout_stream.get(), *out_module);
    }
  }
  return result;
}

static void InitEnvironment(Environment* env) {
  HostModule* host_module = env->AppendHostModule("env");
  host_module->import_delegate.reset(new LibcHostImportDelegate(s_stdout_stream.get(), env));

  if (s_disable_jit) {
    env->enable_jit = false;
  }
  if (s_trap_on_failed_comp) {
    env->trap_on_failed_comp = true;
  }

  env->jit_threshold = s_jit_threshold;
}

static wabt::Result ReadAndRunModule(const char* module_filename) {
  wabt::Result result;
  Environment env;
  InitEnvironment(&env);

  ErrorHandlerFile error_handler(Location::Type::Binary);
  DefinedModule* module = nullptr;
  result = ReadModule(module_filename, &env, &error_handler, &module);
  if (Succeeded(result)) {
    Executor executor(&env, s_trace_stream, s_thread_options);
    ExecResult exec_result = executor.RunStartFunction(module);
    if (exec_result.result == interp::Result::Ok) {
      RunStart(module, &env, &executor, RunVerbosity::Verbose);
    } else {
      WriteResult(s_stdout_stream.get(), "error running start function",
                  exec_result.result);
      exec_result.PrintCallStack(s_stdout_stream.get(), &env);
    }
  }
  return result;
}

int ProgramMain(int argc, char** argv) {
  InitStdio();
  s_stdout_stream = FileStream::CreateStdout();

  ParseOptions(argc, argv);

  wabt::Result result = ReadAndRunModule(s_infile);
  return result != wabt::Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
