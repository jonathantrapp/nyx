#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
class Print {
 public:
  virtual ~Print()=default;
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t* data,size_t n) { size_t done=0; for(size_t i=0;i<n;i++) done+=write(data[i]); return done; }
  size_t printf(const char* fmt,...) {
    char buf[2048]; va_list args; va_start(args,fmt);
    int n=vsnprintf(buf,sizeof(buf),fmt,args); va_end(args);
    if(n<0 || static_cast<size_t>(n)>=sizeof(buf)) return 0;
    return write(reinterpret_cast<const uint8_t*>(buf),static_cast<size_t>(n));
  }
};
