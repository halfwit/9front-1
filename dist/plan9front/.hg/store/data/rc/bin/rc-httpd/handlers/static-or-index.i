         Q   P      	5��������`�DMg��±	��Ú�^�uQ            u#!/bin/rc
if(~ $PATH_INFO */)
	exec dir-index $params
if not
	exec serve-static
