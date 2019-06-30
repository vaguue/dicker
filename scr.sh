#!/bin/bash

#make && \
#  ./a.out


gcc $1 -g &&
  ./a.out -d localhost -p 1488 -f sender.c
