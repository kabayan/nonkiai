/* ----------------------------------------------------------------- */
/* DLS-174 HTS フロント整備: JPCommon フロントアダプタ実装            */
/*                                                                   */
/* convert_pos / convert_ctype / convert_cform は Open JTalk 1.11    */
/* njd2jpcommon.c (Modified BSD, Copyright (c) 2008-2016 Nagoya       */
/* Institute of Technology) から移植 (無改変ロジック)。出典の         */
/* ライセンスは ../jpcommon/LICENSE.open_jtalk / UPSTREAM.md を参照。  */
/* njd.h 依存を避けるため njd2jpcommon() 本体は移植せず、NJDNode から  */
/* の取り出しを引数渡しに置換した jpcommon_front_push を提供する。     */
/* ----------------------------------------------------------------- */
#include "jpcommon_front.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpcommon.h"                  /* upstream */
#include "njd2jpcommon_rule_utf_8.h"   /* upstream: njd2jpcommon_{pos,ctype,cform}_list */

#define MAXBUFLEN 1024

/* --- 以下3関数は upstream njd2jpcommon.c より移植 (BSD) --- */

static void convert_pos(char *buff, const char *pos, const char *pos_group1,
                        const char *pos_group2, const char *pos_group3)
{
   int i;

   for (i = 0; njd2jpcommon_pos_list[i] != NULL; i += 5) {
      if (strcmp(njd2jpcommon_pos_list[i], pos) == 0 &&
          strcmp(njd2jpcommon_pos_list[i + 1], pos_group1) == 0 &&
          strcmp(njd2jpcommon_pos_list[i + 2], pos_group2) == 0 &&
          strcmp(njd2jpcommon_pos_list[i + 3], pos_group3) == 0) {
         strcpy(buff, njd2jpcommon_pos_list[i + 4]);
         return;
      }
   }
   fprintf(stderr,
           "WARNING: convert_pos() in jpcommon_front.cpp: %s %s %s %s are not appropriate POS.\n",
           pos, pos_group1, pos_group2, pos_group3);
   strcpy(buff, njd2jpcommon_pos_list[4]);
}

static void convert_ctype(char *buff, const char *ctype)
{
   int i;

   for (i = 0; njd2jpcommon_ctype_list[i] != NULL; i += 2) {
      if (strcmp(njd2jpcommon_ctype_list[i], ctype) == 0) {
         strcpy(buff, njd2jpcommon_ctype_list[i + 1]);
         return;
      }
   }
   fprintf(stderr,
           "WARNING: convert_ctype() in jpcommon_front.cpp: %s is not appropriate conjugation type.\n",
           ctype);
   strcpy(buff, njd2jpcommon_ctype_list[1]);
}

static void convert_cform(char *buff, const char *cform)
{
   int i;

   for (i = 0; njd2jpcommon_cform_list[i] != NULL; i += 2) {
      if (strcmp(njd2jpcommon_cform_list[i], cform) == 0) {
         strcpy(buff, njd2jpcommon_cform_list[i + 1]);
         return;
      }
   }
   fprintf(stderr,
           "WARNING: convert_cform() in jpcommon_front.cpp: %s is not appropriate conjugation form.\n",
           cform);
   strcpy(buff, njd2jpcommon_cform_list[1]);
}

/* --- upstream njd2jpcommon() のループ本体を引数渡しに置換 --- */

void jpcommon_front_push(JPCommon *jpcommon,
                         const char *pron,
                         const char *pos, const char *pos_group1,
                         const char *pos_group2, const char *pos_group3,
                         const char *ctype, const char *cform,
                         int acc, int chain_flag)
{
   char buff[MAXBUFLEN];
   JPCommonNode *jnode = (JPCommonNode *) calloc(1, sizeof(JPCommonNode));
   JPCommonNode_initialize(jnode);
   JPCommonNode_set_pron(jnode, pron);
   convert_pos(buff, pos, pos_group1, pos_group2, pos_group3);
   JPCommonNode_set_pos(jnode, buff);
   convert_ctype(buff, ctype);
   JPCommonNode_set_ctype(jnode, buff);
   convert_cform(buff, cform);
   JPCommonNode_set_cform(jnode, buff);
   JPCommonNode_set_acc(jnode, acc);
   JPCommonNode_set_chain_flag(jnode, chain_flag);
   JPCommon_push(jpcommon, jnode);
}
