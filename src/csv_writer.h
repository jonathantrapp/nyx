// csv_writer.h - turns one CaptureRow into one line of CSV.
#pragma once
#include <Arduino.h>
#include "capture.h"

// First line of every session file. Your csvWriteRow() must emit columns in this order.
extern const char CSV_HEADER[];

// Flip to true in csv_writer.cpp once csvWriteRow() is implemented. While false the
// OLED and serial status say "CSV STUB" so you never mistake an empty log for a quiet room.
extern const bool CSV_WRITER_DONE;

// Write exactly one CSV line (including the trailing '\n') for `row` to `out`.
void csvWriteRow(Print& out, const CaptureRow& row);
