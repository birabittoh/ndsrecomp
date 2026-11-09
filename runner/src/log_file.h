// log_file.h -- a copy of stderr in a log file, laid out like rexglue-sdk's
// (src/core/logging.cpp there):
//
//   <dir>/<app>_NNN.log   NNN one past the highest number already in <dir>
//   [YYYY-MM-DD HH:MM:SS.mmm] <line>   local time, one prefix per line
//   at 5 MB the file rotates to <app>_NNN.1.log, .2, ... and 20 are kept
//
// stderr itself is unchanged: the console or a `2> file` redirect gets the
// same bytes as before, unprefixed.

#pragma once

#include <filesystem>
#include <string>

// First thing in main: route stderr through a pipe so that everything
// written from here on reaches the log, including lines printed before the
// log's directory is known. Until nds_log_file_open those lines are held in
// memory (and dropped if it is never called).
void nds_log_capture_begin();

// Start writing the log. An empty `explicit_path` picks the next sequential
// name in `dir`. On a Windows debugger the lines also go to
// OutputDebugString, as rexglue's msvc sink does.
bool nds_log_file_open(const std::string& explicit_path,
                       const std::filesystem::path& dir,
                       const std::string& app_name);

// The file being written, or empty.
std::string nds_log_file_path();
