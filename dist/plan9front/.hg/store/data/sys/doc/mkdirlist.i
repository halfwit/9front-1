         C   B      �������������������#T�:@�_�            u#!/bin/rc
# mkdirlist
for(i in $*)
	if(~ $i */*)
		basename -d $i
