/* $Id: make-js-actions.c 5675 2010-01-19 09:52:11Z cher $ */

#include <stdio.h>
#include "new-server.h"

#define A(n) [n] = #n
const unsigned char * const action_table[NEW_SRV_ACTION_LAST] =
{
  A(NEW_SRV_ACTION_VIEW_PROBLEM_SUMMARY),
  A(NEW_SRV_ACTION_VIEW_PROBLEM_SUBMIT),
  A(NEW_SRV_ACTION_JSON_USER_STATE),
  A(NEW_SRV_ACTION_UPDATE_ANSWER),
};

int main(void)
{
  int i;

  for (i = 0; i < NEW_SRV_ACTION_LAST; i++)
    if (action_table[i])
      printf("var %s=%d;\n", action_table[i], i);

  return 0;
}
