#!/bin/sh

grep -o 'mwaitx\|waitpkg' /proc/cpuinfo | sort -u
