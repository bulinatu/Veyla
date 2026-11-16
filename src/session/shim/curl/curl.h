#pragma once
// Minimal stand-in for libcurl's header. chiaki's AIA helpers only need the
// CURL type to compile; this build does not use AIA certificate fetching.
typedef void CURL;
