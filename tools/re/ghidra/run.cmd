@echo off
rem Run a Ghidra script headless against the analyzed Starfield.exe 1.16.244 project.
rem usage: run.cmd <Script.java> <output file> <args...>
rem Needs Ghidra 12.2 + JDK 25 under C:\Tools and the project C:\Tools\ghidra-projects\sf (see the memory note
rem ghidra-starfield-setup). user.home etc. are redirected because Ghidra's OSGi fails under the packaged app's AppData.
set JAVA_HOME=C:\Tools\jdk-25.0.5.1+2
set GHIDRA_HEADLESS_MAXMEM=12G
set JDK_JAVA_OPTIONS=-Duser.home=C:\Tools\ghidra-home -Dapplication.settingsdir=C:\Tools\ghidra-home\settings -Dapplication.cachedir=C:\Tools\ghidra-home\cache -Dapplication.tempdir=C:\Tools\ghidra-home\temp
call C:\Tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat C:\Tools\ghidra-projects\sf Starfield -process Starfield.exe -noanalysis -readOnly -scriptPath "%~dp0." -postScript %*
