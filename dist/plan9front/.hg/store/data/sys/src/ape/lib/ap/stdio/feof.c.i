         ]   \        ��������UA�]mn�E5�¹�=����            u/*
 * pANS stdio -- feof
 */
#include "iolib.h"
int feof(FILE *f){
	return f->state==END;
}
