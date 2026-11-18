// commands.h - line-based serial console ("help" lists everything).
#pragma once

void commandsService();   // call every loop(); handles at most one complete line
void commandsPrintHelp();
