#!/usr/bin/env python
#
# Modified from: run-interp.py
# Modified to compare the output and execution time of the JIT and the
# interpreter. Minor additional modifications to allow the passing of .wasm
# files directly.
#
# Copyright 2016 WebAssembly Community Group participants
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import argparse
import os
import sys
import time
import difflib

import find_exe
import utils
from utils import Error

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def main(args):
  parser = argparse.ArgumentParser()
  parser.add_argument('-o', '--out-dir', metavar='PATH',
                      help='output directory for files.')
  parser.add_argument('-v', '--verbose', help='print more diagnotic messages.',
                      action='store_true')
  parser.add_argument('--bindir', metavar='PATH',
                      default=find_exe.GetDefaultPath(),
                      help='directory to search for all executables.')
  parser.add_argument('--no-error-cmdline',
                      help='don\'t display the subprocess\'s commandline when'
                      + ' an error occurs', dest='error_cmdline',
                      action='store_false')
  parser.add_argument('-p', '--print-cmd',
                      help='print the commands that are run.',
                      action='store_true')
  parser.add_argument('--run-all-exports', action='store_true')
  parser.add_argument('--host-print', action='store_true')
  parser.add_argument('--spec', action='store_true')
  parser.add_argument('-t', '--trace', action='store_true')
  parser.add_argument('file', help='test file.')
  parser.add_argument('--enable-saturating-float-to-int', action='store_true')
  parser.add_argument('--enable-threads', action='store_true')
  parser.add_argument('--disable-jit', action='store_true')
  parser.add_argument('--trap-on-failed-comp', action='store_true')
  options = parser.parse_args(args)

  wast_tool = None
  interp_tool = None
  interp_jit_tool = None
  wast_tool = utils.Executable(
      find_exe.GetWat2WasmExecutable(options.bindir),
      error_cmdline=options.error_cmdline)
  interp_tool = utils.Executable(
      find_exe.GetWasmInterpExecutable(options.bindir),
      error_cmdline=options.error_cmdline)
  interp_tool.AppendOptionalArgs({
      '--host-print': options.host_print,
      '--run-all-exports': options.run_all_exports,
  })

  interp_jit_tool = utils.Executable(
      find_exe.GetWasmInterpExecutable(options.bindir),
      error_cmdline=options.error_cmdline)
  interp_jit_tool.AppendOptionalArgs({
      '--host-print': options.host_print,
      '--run-all-exports': options.run_all_exports,
  })

  wast_tool.AppendOptionalArgs({
      '-v': options.verbose,
      '--enable-saturating-float-to-int':
          options.enable_saturating_float_to_int,
      '--enable-threads': options.enable_threads,
  })

  interp_tool.AppendOptionalArgs({
      '-v': options.verbose,
      '--run-all-exports': options.run_all_exports,
      '--trace': options.trace,
      '--enable-saturating-float-to-int':
          options.enable_saturating_float_to_int,
      '--enable-threads': options.enable_threads,
      '--disable-jit': True,
  })
  interp_jit_tool.AppendOptionalArgs({
      '-v': options.verbose,
      '--run-all-exports': options.run_all_exports,
      '--trace': options.trace,
      '--enable-saturating-float-to-int':
          options.enable_saturating_float_to_int,
      '--enable-threads': options.enable_threads,
      '--trap-on-failed-comp': options.trap_on_failed_comp,
  })

  wast_tool.verbose = options.print_cmd
  interp_tool.verbose = options.print_cmd
  interp_jit_tool.verbose = options.print_cmd

  with utils.TempDirectory(options.out_dir, 'run-interp-') as out_dir:
    if not options.file.endswith('.wasm'):
        new_ext = '.json' if options.spec else '.wasm'
        out_file = utils.ChangeDir(
            utils.ChangeExt(options.file, new_ext), out_dir)
        wast_tool.RunWithArgs(options.file, '-o', out_file)
    else:
        out_file = options.file
    start = time.time()
    interp_out = interp_tool.RunWithArgsForStdout(out_file)
    interp_time = time.time() - start
    start = time.time()
    jit_out = interp_jit_tool.RunWithArgsForStdout(out_file)
    jit_time = time.time() - start
    print("Interpreter: {}\nJIT: {}".format(interp_time, jit_time))
    expected_lines = [line for line in interp_out.splitlines() if line]
    actual_lines = [line for line in jit_out.splitlines() if line]
    diff_lines = list(
        difflib.unified_diff(expected_lines, actual_lines, fromfile='expected',
                             tofile='actual', lineterm=''))
    msg = ""
    if len(diff_lines) > 0:
      msg += 'STDOUT MISMATCH:\n' + '\n'.join(diff_lines) + '\n'

    if msg:
      raise Error(msg)

  return 0


if __name__ == '__main__':
  try:
    sys.exit(main(sys.argv[1:]))
  except Error as e:
    sys.stderr.write(str(e) + '\n')
    sys.exit(1)
