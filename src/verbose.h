/*
 Copyright 2015 IoT.bzh

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/


extern int verbosity;
#define warning(...) do{if(verbosity)syslog(LOG_WARNING,__VA_ARGS__);}while(0)
#define notice(...)  do{if(verbosity)syslog(LOG_NOTICE,__VA_ARGS__);}while(0)
#define info(...)    do{if(verbosity)syslog(LOG_INFO,__VA_ARGS__);}while(0)
#define debug(...)   do{if(verbosity>1)syslog(LOG_DEBUG,__VA_ARGS__);}while(0)
extern int verbose_scan_args(int argc, char **argv);
