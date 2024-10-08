/*
** Copyright (c) 2024 Zenkj
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   juzejian@gmail.com
**   http://www.frybox.org
**
*******************************************************************************
**
** This file contains code to implement the end-to-end encrypted
** file transfer protocol.
*/
#include "config.h"
#include "exfer.h"

#include <time.h>

/*
** Maximum number of HTTP redirects that any http_exchange() call will
** follow before throwing a fatal error. Most browsers use a limit of 20.
*/
#define MAX_REDIRECTS 20

/*
** This structure holds information about the current state of either
** a client or a server that is participating in xfer.
*/
typedef struct Xfer Xfer;
struct Xfer {
  Blob *pIn;          /* Input text from the other side */
  Blob *pOut;         /* Compose our reply here */
  Blob line;          /* The current line of input */
  Blob aToken[6];     /* Tokenized version of line */
  Blob err;           /* Error message text */
  int nToken;         /* Number of tokens in line */
  int nHaveSent;      /* Number of "have" cards sent */
  int nNeedSent;      /* Number of ineed cards sent */
  int nAtomSent;      /* Number of atom nodes sent */
  int nListSent;      /* Number of list nodes sent */
  int nAtomRcvd;      /* Number of atom nodes received */
  int nListRcvd;      /* Number of list nodes received */
  int mxSend;         /* Stop sending "file" when pOut reaches this size */
  int resync;         /* Send have cards for all holdings */
  u32 peerVersion;    /* Version of fossil running on the other side */
  u32 peerDate;       /* Date for specific client software edition */
  u32 peerTime;       /* Time of date correspoding on peerDate */   
  time_t maxTime;     /* Time when this transfer should be finished */
};


/*
** The input blob contains an artifact.  Convert it into a record ID.
** Create a phantom record if no prior record exists and
** phantomize is true.
**
** Compare to uuid_to_rid().  This routine takes a blob argument
** and does less error checking.
*/
static int nid_from_uuid(Blob *pUuid, int partial, int isPrivate){
  static Stmt q;
  int rid;

  db_static_prepare(&q, "SELECT nid FROM node WHERE uuid=:uuid");
  db_bind_str(&q, ":uuid", pUuid);
  if( db_step(&q)==SQLITE_ROW ){
    rid = db_column_int(&q, 0);
  }else{
    rid = 0;
  }
  db_reset(&q);
  if( rid==0 && phantomize ){
    rid = content_new(blob_str(pUuid), isPrivate);
  }
  return rid;
}

/*
** Remember that the other side of the connection already has a copy
** of the node nid.
*/
static void peer_have(int nid){
  if( nid ){
    static Stmt q;
    db_static_prepare(&q, "INSERT OR IGNORE INTO peerhave VALUES(:n)");
    db_bind_int(&q, ":n", nid);
    db_step(&q);
    db_reset(&q);
  }
}

/*
** Remember that the other side of the connection lacks a copy of
** the artifact with the given hash.
*/
static void peer_need(Blob *pHash){
  static Stmt q;
  db_static_prepare(&q, "INSERT OR IGNORE INTO peerneed VALUES(:h)");
  db_bind_text(&q, ":h", blob_str(pHash));
  db_step(&q);
  db_reset(&q);
}

/*
** The aToken[0..nToken-1] blob array is a parse of a "atom" line
** message.  This routine finishes parsing that message and does
** a record insert of the atom node if it is not partial.
**
** The atom line is in the following form:
**
**      atom HASH SIZE BOFFSET EOFFSET\n CONTENT
**
** The CONTENT is (EOFFSET-BOFFSET) bytes immediately following
** the newline.
**
** SIZE is the total size of the atom node.  BOFFSET is the begin
** offset of the CONTENT within the atom node.  EOFFSET is the
** end offset of the CONTENT within the atom node.
**
** When the atom node is in the same message, BOFFSET is always zero,
** and EOFFSET is the same as SIZE.
**
** If any error occurs, write a message into pErr which has already
** be initialized to an empty string.
*/
static void xfer_accept_atom_node(
  Xfer *pXfer,
  int cloneFlag
){
  int size;
  int boffset;
  int eoffset;
  int rid;
  int srcid = 0;
  Blob content;
  int isPriv;
  Blob *pUuid;

  if( pXfer->nToken != 5
   || !blob_is_hname(&pXfer->aToken[1])
   || !blob_is_int(&pXfer->aToken[2], &size)
   || !blob_is_int(&pXfer->aToken[3], &boffset)
   || !blob_is_int(&pXfer->aToken[4], &eoffset)
   || size < 0
   || boffset < 0
   || eoffset < 0
   || eoffset > size
  ){
    blob_appendf(&pXfer->err, "malformed atom line");
    return;
  }
  blob_zero(&content);
  blob_extract(pXfer->pIn, (eoffset-boffset), &content);
  pUuid = &pXfer->aToken[1];
  if( boffset==0 && eoffset==size ){
    // This is a complete atom node, write to nodes directly.
    if( hname_verify_hash(&content, blob_buffer(pUuid), blob_size(pUuid))==0 ){
      blob_appendf(&pXfer->err, "wrong hash on received atom: %b", pUuid);
    }
  }else{
    // TODO 找到如何发现partial文件路径的方法
  }





  if( cloneFlag ){
    if( pXfer->nToken==4 ){
      srcid = nid_from_uuid(&pXfer->aToken[2], 1, isPriv);
      pXfer->nDeltaRcvd++;
    }else{
      srcid = 0;
      pXfer->nFileRcvd++;
    }
    rid = content_put_ex(&content, blob_str(pUuid), srcid,
                         0, isPriv);
    peer_have(rid);
    blob_reset(&content);
    return;
  }
  if( pXfer->nToken==4 ){
    Blob src, next;
    srcid = nid_from_uuid(&pXfer->aToken[2], 1, isPriv);
    if( content_get(srcid, &src)==0 ){
      rid = content_put_ex(&content, blob_str(pUuid), srcid,
                           0, isPriv);
      pXfer->nDanglingFile++;
      db_multi_exec("DELETE FROM phantom WHERE rid=%d", rid);
      if( !isPriv ) content_make_public(rid);
      blob_reset(&src);
      blob_reset(&content);
      return;
    }
    pXfer->nDeltaRcvd++;
    blob_delta_apply(&src, &content, &next);
    blob_reset(&src);
    blob_reset(&content);
    content = next;
  }else{
    pXfer->nFileRcvd++;
  }
  if( hname_verify_hash(&content, blob_buffer(pUuid), blob_size(pUuid))==0 ){
    blob_appendf(&pXfer->err, "wrong hash on received artifact: %b", pUuid);
  }
  rid = content_put_ex(&content, blob_str(pUuid), 0, 0, isPriv);
  if( rid==0 ){
    blob_appendf(&pXfer->err, "%s", g.zErrMsg);
    blob_reset(&content);
  }else{
    if( !isPriv ) content_make_public(rid);
    manifest_crosslink(rid, &content, MC_NO_ERRORS);
  }
  assert( blob_is_reset(&content) );
  peer_have(rid);
}

/*
** Send the file identified by rid.
**
** The pUuid can be NULL in which case the correct hash is computed
** from the rid.
**
** Try to send the file as a native delta if nativeDelta is true, or
** as a parent delta if nativeDelta is false.
**
** It should never be the case that rid is a private artifact.  But
** as a precaution, this routine does check on rid and if it is private
** this routine becomes a no-op.
*/
static void send_node(Xfer *pXfer, int rid, Blob *pUuid, int nativeDelta){
  Blob content, uuid;
  int size = 0;
  int isPriv = content_is_private(rid);

  if( isPriv && pXfer->syncPrivate==0 ){
    if( pXfer->remoteDate>=20200413 && pUuid && blob_size(pUuid)>0 ){
      /* If the artifact is private and we are not doing a private sync,
      ** at least tell the other side that the artifact exists and is
      ** known to be private.  But only do this for newer clients since
      ** older ones will throw an error if they get a private have card
      ** and private syncing is disallowed */
      blob_appendf(pXfer->pOut, "have %b 1\n", pUuid);
      pXfer->nIGotSent++;
      pXfer->nPrivIGot++;
    }
    return;
  }
  if( db_exists("SELECT 1 FROM peerhave WHERE rid=%d", rid) ){
     return;
  }
  blob_zero(&uuid);
  db_blob(&uuid, "SELECT uuid FROM blob WHERE rid=%d AND size>=0", rid);
  if( blob_size(&uuid)==0 ){
    return;
  }
  if( blob_size(&uuid)>HNAME_LEN_SHA1 && pXfer->remoteVersion<20000 ){
    xfer_cannot_send_sha3_error(pXfer);
    return;
  }
  if( pUuid ){
    if( blob_compare(pUuid, &uuid)!=0 ){
      blob_reset(&uuid);
      return;
    }
  }else{
    pUuid = &uuid;
  }
  if( uuid_is_shunned(blob_str(pUuid)) ){
    blob_reset(&uuid);
    return;
  }
  if( (pXfer->maxTime != -1 && time(NULL) >= pXfer->maxTime) ||
       pXfer->mxSend<=(int)blob_size(pXfer->pOut) ){
    const char *zFormat = isPriv ? "have %b 1\n" : "have %b\n";
    blob_appendf(pXfer->pOut, zFormat /*works-like:"%b"*/, pUuid);
    pXfer->nIGotSent++;
    blob_reset(&uuid);
    return;
  }
  if( nativeDelta ){
    size = send_delta_native(pXfer, rid, isPriv, pUuid);
    if( size ){
      pXfer->nDeltaSent++;
    }
  }
  if( size==0 ){
    content_get(rid, &content);

    if( !nativeDelta && blob_size(&content)>100 ){
      size = send_delta_parent(pXfer, rid, isPriv, &content, pUuid);
    }
    if( size==0 ){
      int size = blob_size(&content);
      if( isPriv ) blob_append(pXfer->pOut, "private\n", -1);
      blob_appendf(pXfer->pOut, "file %b %d\n", pUuid, size);
      blob_append(pXfer->pOut, blob_buffer(&content), size);
      pXfer->nFileSent++;
    }else{
      pXfer->nDeltaSent++;
    }
    blob_reset(&content);
  }
  peer_have(rid);
  blob_reset(&uuid);
#if 0
  if( blob_buffer(pXfer->pOut)[blob_size(pXfer->pOut)-1]!='\n' ){
    blob_append(pXfer->pOut, "\n", 1);
  }
#endif
}

/*
** Send a INEED card for every partial node.
**
** This is the only place that send INEED cards.
*/
static void request_partials(Xfer *pXfer, int maxReq){
  Stmt q;
  db_prepare(&q,
    "SELECT uuid, size FROM partial"
    " WHERE NOT EXISTS(SELECT 1 FROM peerneed WHERE peerneed.uuid=partial.uuid)"
  );
  while( db_step(&q)==SQLITE_ROW && maxReq-- > 0 ){
    const char *zUuid = db_column_text(&q, 0);
    int size = db_column_int(&q, 1);
    blob_appendf(pXfer->pOut, "ineed %s %d\n", zUuid, size);
    pXfer->nINeedSent++;
  }
  db_finalize(&q);
}

/*
** Compute an hash on the tail of pMsg.  Verify that it matches the
** the hash given in pHash.  Return non-zero for an error and 0 on success.
**
** The type of hash computed (SHA1, SHA3-256) is determined by
** the length of the input hash in pHash.
*/
static int check_tail_hash(Blob *pHash, Blob *pMsg){
  Blob tail;
  int rc;
  blob_tail(pMsg, &tail);
  rc = hname_verify_hash(&tail, blob_buffer(pHash), blob_size(pHash));
  blob_reset(&tail);
  return rc==HNAME_ERROR;
}

/*
** Check the signature on an application/x-fossil payload received by
** the HTTP server.  The signature is a line of the following form:
**
**        login LOGIN NONCE SIGNATURE
**
** The NONCE is the SHA1 hash of the remainder of the input.
** SIGNATURE is the SHA1 checksum of the NONCE concatenated
** with the sha1_shared_secret() encoding of the users password.
**
**   SIGNATURE = sha1_sum( NONCE + sha1_shared_secret(PASSWORD) );
**
** The parameters to this routine are ephemeral blobs holding the
** LOGIN, NONCE and SIGNATURE.
**
** This routine attempts to locate the user and verify the signature.
** If everything checks out, the USER.CAP column for the USER table
** is consulted to set privileges in the global g variable.
**
** If anything fails to check out, no changes are made to privileges.
**
** Signature generation on the client side is handled by the
** http_exchange() routine.
**
** Return non-zero for a login failure and zero for success.
*/
static int check_login(Blob *pLogin, Blob *pNonce, Blob *pSig){
  Stmt q;
  int rc = -1;
  char *zLogin = blob_terminate(pLogin);
  defossilize(zLogin);

  if( fossil_strcmp(zLogin, "nobody")==0
   || fossil_strcmp(zLogin,"anonymous")==0
  ){
    return 0;   /* Anybody is allowed to sync as "nobody" or "anonymous" */
  }
  if( fossil_strcmp(P("REMOTE_USER"), zLogin)==0
      && db_get_boolean("remote_user_ok",0) ){
    return 0;   /* Accept Basic Authorization */
  }
  db_prepare(&q,
     "SELECT pw, cap, uid FROM user"
     " WHERE login=%Q"
     "   AND login NOT IN ('anonymous','nobody','developer','reader')"
     "   AND length(pw)>0",
     zLogin
  );
  if( db_step(&q)==SQLITE_ROW ){
    int szPw;
    Blob pw, combined, hash;
    blob_zero(&pw);
    db_ephemeral_blob(&q, 0, &pw);
    szPw = blob_size(&pw);
    blob_zero(&combined);
    blob_copy(&combined, pNonce);
    blob_append(&combined, blob_buffer(&pw), szPw);
    sha1sum_blob(&combined, &hash);
    assert( blob_size(&hash)==40 );
    rc = blob_constant_time_cmp(&hash, pSig);
    blob_reset(&hash);
    blob_reset(&combined);
    if( rc!=0 && szPw!=40 ){
      /* If this server stores cleartext passwords and the password did not
      ** match, then perhaps the client is sending SHA1 passwords.  Try
      ** again with the SHA1 password.
      */
      const char *zPw = db_column_text(&q, 0);
      char *zSecret = sha1_shared_secret(zPw, blob_str(pLogin), 0);
      blob_zero(&combined);
      blob_copy(&combined, pNonce);
      blob_append(&combined, zSecret, -1);
      free(zSecret);
      sha1sum_blob(&combined, &hash);
      rc = blob_constant_time_cmp(&hash, pSig);
      blob_reset(&hash);
      blob_reset(&combined);
    }
    if( rc==0 ){
      const char *zCap;
      zCap = db_column_text(&q, 1);
      login_set_capabilities(zCap, 0);
      g.userUid = db_column_int(&q, 2);
      g.zLogin = mprintf("%b", pLogin);
      g.zNonce = mprintf("%b", pNonce);
    }
  }
  db_finalize(&q);
  return rc;
}

/*
** Send the content of all files in the unsent table.
**
** This is really just an optimization.  If you clear the
** unsent table, all the right files will still get transferred.
** It just might require an extra round trip or two.
*/
static void send_unsent(Xfer *pXfer){
  Stmt q;
  db_prepare(&q, "SELECT rid FROM unsent EXCEPT SELECT rid FROM private");
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q, 0);
    send_file(pXfer, rid, 0, 0);
  }
  db_finalize(&q);
  db_multi_exec("DELETE FROM unsent");
}

/*
** Check to see if the number of root entries is greater than
** 100 and if it is, form a new list.
*/
void create_list_node(void){
  Blob listnode, cksum;
  Blob deleteWhere;
  Stmt q;
  int nUncl;
  int nRow = 0;
  int rid;

#if 0
  /* We should not ever get any private artifacts in the unclustered table.
  ** But if we do (because of a bug) now is a good time to delete them. */
  db_multi_exec(
    "DELETE FROM unclustered WHERE rid IN (SELECT rid FROM private)"
  );
#endif

  nUncl = db_int(0, "SELECT count(*) FROM root /*scan*/");
  if( nUncl>=100 ){
    blob_zero(&listnode);
    blob_zero(&deleteWhere);
    db_prepare(&q, "SELECT uuid FROM root, node"
                   " WHERE root.nid=node.nid"
                   " ORDER BY 1");
    while( db_step(&q)==SQLITE_ROW ){
      blob_appendf(&listnode, "M %s\n", db_column_text(&q, 0));
      nRow++;
      if( nRow>=800 && nUncl>nRow+100 ){
        md5sum_blob(&listnode, &cksum);
        blob_appendf(&listnode, "Z %b\n", &cksum);
        blob_reset(&cksum);
        // TODO 改为保存到node表和nodes文件中
        // rid = content_put(&listnode);
        blob_reset(&listnode);
        nUncl -= nRow;
        nRow = 0;
        blob_append_sql(&deleteWhere, ",%d", rid);
      }
    }
    db_finalize(&q);
    db_multi_exec(
      "DELETE FROM root WHERE nid NOT IN (0 %s)",
      blob_sql_text(&deleteWhere)
    );
    blob_reset(&deleteWhere);
    if( nRow>0 ){
      md5sum_blob(&listnode, &cksum);
      blob_appendf(&listnode, "Z %b\n", &cksum);
      blob_reset(&cksum);
      // TODO 改为保存到node表和nodes文件中
      // rid = content_put(&listnode);
      blob_reset(&listnode);
    }
  }
}

/*
** Send igot messages for every private artifact
*/
static int send_private(Xfer *pXfer){
  int cnt = 0;
  Stmt q;
  if( pXfer->syncPrivate ){
    db_prepare(&q, "SELECT uuid FROM private JOIN blob USING(rid)");
    while( db_step(&q)==SQLITE_ROW ){
      blob_appendf(pXfer->pOut, "igot %s 1\n", db_column_text(&q,0));
      cnt++;
    }
    db_finalize(&q);
  }
  return cnt;
}

/*
** Send an igot message for every entry in unclustered table.
** Return the number of cards sent.
**
** Except:
**    *  Do not send igot cards for shunned artifacts
**    *  Do not send igot cards for phantoms
**    *  Do not send igot cards for private artifacts
**    *  Do not send igot cards for any artifact that is in the
**       ONREMOTE table, if that table exists.
**
** If the pXfer->resync flag is set, that means we are doing a "--verily"
** sync and all artifacts that don't meet the restrictions above should
** be sent.
*/
static int send_root(Xfer *pXfer){
  Stmt q;
  int cnt = 0;
  const char *zExtra;
  if( db_table_exists("temp","peerhave") ){
    zExtra = " AND NOT EXISTS(SELECT 1 FROM peerhave WHERE uuid=node.uuid)";
  }else{
    zExtra = "";
  }
  if( pXfer->resync ){
    db_prepare(&q,
      "SELECT uuid, nid FROM node"
      " WHERE node.nid<=%d%s"
      " ORDER BY node.nid DESC",
      pXfer->resync, zExtra /*safe-for-%s*/
    );
  }else{
    db_prepare(&q,
      "SELECT uuid FROM root JOIN node USING(nid) /*scan*/"
      " WHERE 1%s",
      zExtra /*safe-for-%s*/
    );
  }
  while( db_step(&q)==SQLITE_ROW ){
    blob_appendf(pXfer->pOut, "have %s\n", db_column_text(&q, 0));
    cnt++;
    if( pXfer->resync && pXfer->mxSend<(int)blob_size(pXfer->pOut) ){
      pXfer->resync = db_column_int(&q, 1)-1;
    }
  }
  db_finalize(&q);
  if( cnt==0 ) pXfer->resync = 0;
  return cnt;
}

/*
** Send an have message for every node.
*/
static void send_all(Xfer *pXfer){
  Stmt q;
  db_prepare(&q,
    "SELECT uuid FROM node"
  );
  while( db_step(&q)==SQLITE_ROW ){
    blob_appendf(pXfer->pOut, "have %s\n", db_column_text(&q, 0));
  }
  db_finalize(&q);
}

/*
** This routine makes a "syncwith:URL" entry in the CONFIG table to
** indicate that a sync is occuring with zUrl.
**
** Add a "syncfrom:URL" entry instead of "syncwith:URL" if bSyncFrom is true.
*/
static void xfer_syncwith(const char *zUrl, int bSyncFrom){
  UrlData x;
  memset(&x, 0, sizeof(x));
  url_parse_local(zUrl, URL_OMIT_USER, &x);
  if( x.protocol && strncmp(x.protocol,"http",4)==0
   && x.name && sqlite3_strlike("%localhost%", x.name, 0)!=0
  ){
    db_unprotect(PROTECT_CONFIG);
    db_multi_exec("REPLACE INTO config(name,value,mtime)"
                  "VALUES('sync%q:%q','{}',now())",
       bSyncFrom ? "from" : "with", x.canonical);
    db_protect_pop();
  }
  url_unparse(&x);
}

/*
** If this variable is set, disable login checks.  Used for debugging
** only.
*/
static int disableLogin = 0;

/*
** The CGI/HTTP preprocessor always redirects requests with a content-type
** of application/x-frybox or application/x-frybox-debug to this page,
** regardless of what path was specified in the HTTP header.  This allows
** clone clients to specify a URL that omits default pathnames, such
** as "http://fossil-scm.org/" instead of "http://fossil-scm.org/index.cgi".
**
** WEBPAGE: xfer  raw-content  loadavg-exempt
**
** This is the transfer handler on the server side.  The transfer
** message has been uncompressed and placed in the g.cgiIn blob.
** Process this message and form an appropriate reply.
*/
void page_xfer(void){
  int isPull = 0;
  int isPush = 0;
  int nErr = 0;
  Xfer xfer;
  int deltaFlag = 0;
  int isClone = 0;
  int nNeed = 0;
  int size;
  char *zNow;
  int rc;
  const char *zScript = 0;
  int uvCatalogSent = 0;
  int bSendLinks = 0;

  if( fossil_strcmp(PD("REQUEST_METHOD","POST"),"POST") ){
     fossil_redirect_home();
  }
  g.zLogin = "anonymous";
  login_set_anon_nobody_capabilities();
  login_check_credentials();
  cgi_check_for_malice();
  memset(&xfer, 0, sizeof(xfer));
  blobarray_zero(xfer.aToken, count(xfer.aToken));
  cgi_set_content_type(g.zContentType);
  cgi_reset_content();
  if( db_schema_is_outofdate() ){
    @ error database\sschema\sis\sout-of-date\son\sthe\sserver.
    return;
  }
  blob_zero(&xfer.err);
  xfer.pIn = &g.cgiIn;
  xfer.pOut = cgi_output_blob();
  xfer.mxSend = db_get_int("max-download", 5000000);
  xfer.maxTime = db_get_int("max-download-time", 30);
  if( xfer.maxTime<1 ) xfer.maxTime = 1;
  xfer.maxTime += time(NULL);
  g.xferPanic = 1;

  db_begin_write();
  db_multi_exec(
     "CREATE TEMP TABLE peerhave(uuid TEXT PRIMARY KEY) WITHOUT ROWID;"
     "CREATE TEMP TABLE peerneed(uuid TEXT PRIMARY KEY) WITHOUT ROWID;"
  );
  manifest_crosslink_begin();
  while( blob_line(xfer.pIn, &xfer.line) ){
    if( blob_buffer(&xfer.line)[0]=='#' ) continue;
    if( blob_size(&xfer.line)==0 ) continue;
    xfer.nToken = blob_tokenize(&xfer.line, xfer.aToken, count(xfer.aToken));

    /*   atom HASH SIZE \n CONTENT
    **
    ** Server accepts a atom node from the client.
    */
    if( blob_eq(&xfer.aToken[0], "atom") ){
      if( !isPush ){
        cgi_reset_content();
        @ error not\sauthorized\sto\swrite
        nErr++;
        break;
      }
      xfer_accept_atom_node(&xfer, 0);
      if( blob_size(&xfer.err) ){
        cgi_reset_content();
        @ error %T(blob_str(&xfer.err))
        nErr++;
        break;
      }
    }else

    /*   list HASH CSIZE \n CONTENT
    **
    ** Server accepts a list node from the client.
    */
    if( blob_eq(&xfer.aToken[0], "list") ){
      if( !isPush ){
        cgi_reset_content();
        @ error not\sauthorized\sto\swrite
        nErr++;
        break;
      }
      xfer_accept_list_node(&xfer);
      if( blob_size(&xfer.err) ){
        cgi_reset_content();
        @ error %T(blob_str(&xfer.err))
        nErr++;
        break;
      }
    }else

    /*   need HASH
    **
    ** Client is requesting a file from the server.  Send it.
    */
    if( blob_eq(&xfer.aToken[0], "need")
     && xfer.nToken==2
     && blob_is_hname(&xfer.aToken[1])
    ){
      nIneed++;
      remote_unk(&xfer.aToken[1]);
      if( isPull ){
        int rid = nid_from_uuid(&xfer.aToken[1], 0, 0);
        if( rid ){
          send_file(&xfer, rid, &xfer.aToken[1], deltaFlag);
        }
      }
    }else

    /*   have HASH
    **
    ** Client announces that it has a particular file.
    */
    if( xfer.nToken==2
     && blob_eq(&xfer.aToken[0], "have")
     && blob_is_hname(&xfer.aToken[1])
    ){
      if( isPush ){
        int rid = 0;
        rid = nid_from_uuid(&xfer.aToken[1], 1, 0);
        if( rid ){
          peer_have(rid);
        }
      }
    }else


    /*    pull  SERVERCODE  PROJECTCODE
    **    push  SERVERCODE  PROJECTCODE
    **
    ** The client wants either send or receive.  The server should
    ** verify that the project code matches.  The server code is ignored.
    */
    if( xfer.nToken==3
     && (blob_eq(&xfer.aToken[0], "pull") || blob_eq(&xfer.aToken[0], "push"))
     && blob_is_hname(&xfer.aToken[2])
    ){
      const char *zPCode;
      zPCode = db_get("project-code", 0);
      if( zPCode==0 ){
        fossil_fatal("missing project code");
      }
      if( !blob_eq_str(&xfer.aToken[2], zPCode, -1) ){
        cgi_reset_content();
        @ error wrong\sproject
        nErr++;
        break;
      }
      login_check_credentials();
      if( blob_eq(&xfer.aToken[0], "pull") ){
        if( !g.perm.Read ){
          cgi_reset_content();
          @ error not\sauthorized\sto\sread
          nErr++;
          break;
        }
        isPull = 1;
      }else{
        if( !g.perm.Write ){
          if( !isPull ){
            cgi_reset_content();
            @ error not\sauthorized\sto\swrite
            nErr++;
          }else{
            @ message pull\sonly\s-\snot\sauthorized\sto\spush
          }
        }else{
          isPush = 1;
        }
      }
    }else

    /*    clone   ?PROTOCOL-VERSION?  ?SEQUENCE-NUMBER?
    **
    ** The client knows nothing.  Tell all.
    */
    if( blob_eq(&xfer.aToken[0], "clone") ){
      int iVers;
      login_check_credentials();
      if( !g.perm.Clone ){
        cgi_reset_content();
        @ push %s(db_get("server-code", "x")) %s(db_get("project-code", "x"))
        @ error not\sauthorized\sto\sclone
        nErr++;
        break;
      }
      if( db_get_boolean("uv-sync",0) && !uvCatalogSent ){
        @ pragma uv-pull-only
        send_unversioned_catalog(&xfer);
        uvCatalogSent = 1;
      }
      if( xfer.nToken==3
       && blob_is_int(&xfer.aToken[1], &iVers)
       && iVers>=2
      ){
        int seqno, max;
        if( iVers>=3 ){
          cgi_set_content_type("application/x-fossil-uncompressed");
        }
        blob_is_int(&xfer.aToken[2], &seqno);
        max = db_int(0, "SELECT max(rid) FROM blob");
        while( xfer.mxSend>(int)blob_size(xfer.pOut) && seqno<=max){
          if( time(NULL) >= xfer.maxTime ) break;
          if( iVers>=3 ){
            send_compressed_file(&xfer, seqno);
          }else{
            send_file(&xfer, seqno, 0, 1);
          }
          seqno++;
        }
        if( seqno>max ) seqno = 0;
        @ clone_seqno %d(seqno)
      }else{
        isClone = 1;
        isPull = 1;
        deltaFlag = 1;
      }
      @ push %s(db_get("server-code", "x")) %s(db_get("project-code", "x"))
    }else

    /*    login  USER  NONCE  SIGNATURE
    **
    ** The client has sent login credentials to the server.
    ** Validate the login.  This has to happen before anything else.
    ** The client can send multiple logins.  Permissions are cumulative.
    */
    if( blob_eq(&xfer.aToken[0], "login")
     && xfer.nToken==4
    ){
      if( disableLogin ){
        g.perm.Read = g.perm.Write = g.perm.Private = g.perm.Admin = 1;
      }else{
        if( check_tail_hash(&xfer.aToken[2], xfer.pIn)
         || check_login(&xfer.aToken[1], &xfer.aToken[2], &xfer.aToken[3])
        ){
          cgi_reset_content();
          @ error login\sfailed
          nErr++;
          break;
        }
      }
    }else

    /* Unknown message
    */
    {
      cgi_reset_content();
      @ error bad\scommand:\s%F(blob_str(&xfer.line))
    }
    blobarray_reset(xfer.aToken, xfer.nToken);
    blob_reset(&xfer.line);
  }
  if( isPush ){
    request_partials(&xfer, 500);
  }
  if( isClone && nIneed==0 ){
    /* The initial "clone" message from client to server contains no
    ** "gimme" cards. On that initial message, send the client an "igot"
    ** card for every artifact currently in the repository.  This will
    ** cause the client to create phantoms for all artifacts, which will
    ** in turn make sure that the entire repository is sent efficiently
    ** and expeditiously.
    */
    send_all(&xfer);
    if( xfer.syncPrivate ) send_private(&xfer);
  }else if( isPull ){
    create_list_node();
    send_root(&xfer);
    if( xfer.syncPrivate ) send_private(&xfer);
  }
  hook_expecting_more_artifacts(xfer.nGimmeSent?60:0);
  db_multi_exec("DROP TABLE peerhave; DROP TABLE peerneed;");
  manifest_crosslink_end(MC_PERMIT_HOOKS);

  /* Send URLs for alternative repositories for the same project,
  ** if requested by the client. */
  if( bSendLinks && g.zBaseURL ){
    Stmt q;
    db_prepare(&q,
      "WITH remote(mtime, url, arg) AS (\n"
      "  SELECT mtime, substr(name,10), '{}' FROM config\n"
      "   WHERE name GLOB 'syncwith:http*'\n"
      "  UNION ALL\n"
      "  SELECT mtime, substr(name,10), '{}' FROM config\n"
      "   WHERE name GLOB 'syncfrom:http*'\n"
      "  UNION ALL\n"
      "  SELECT mtime, substr(name,9), '{\"type\":\"git\"}' FROM config\n"
      "   WHERE name GLOB 'gitpush:*'\n"
      ")\n"
      "SELECT url, json_insert(arg,'$.src',%Q), max(mtime)\n"
      "  FROM remote WHERE mtime>unixepoch('now','-1 month')\n"
      " GROUP BY url;",
      g.zBaseURL
    );
    while( db_step(&q)==SQLITE_ROW ){
      UrlData x;
      const char *zUrl = db_column_text(&q, 0);
      const char *zArg = db_column_text(&q, 1);
      i64 iMtime = db_column_int64(&q, 2);
      memset(&x, 0, sizeof(x));
      url_parse_local(zUrl, URL_OMIT_USER, &x);
      if( x.name!=0 && sqlite3_strlike("%localhost%", x.name, 0)!=0 ){
        @ pragma link %F(x.canonical) %F(zArg) %lld(iMtime)
      }
      url_unparse(&x);
    }
    db_finalize(&q);
  }

  /* Send the server timestamp last, in case prior processing happened
  ** to use up a significant fraction of our time window.
  */
  zNow = db_text(0, "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%S', 'now')");
  @ # timestamp %s(zNow) errors %d(nErr)
  free(zNow);

  db_commit_transaction();
  configure_rebuild();
}

/*
** COMMAND: test-xfer
**
** Usage: %fossil test-xfer ?OPTIONS? XFERFILE
**
** Pass the sync-protocol input file XFERFILE into the server-side sync
** protocol handler.  Generate a reply on standard output.
**
** This command was original created to help debug the server side of
** sync messages.  The XFERFILE is the uncompressed content of an
** "xfer" HTTP request from client to server.  This command interprets
** that message and generates the content of an HTTP reply (without any
** encoding and without the HTTP reply headers) and writes that reply
** on standard output.
**
** One possible usages scenario is to capture some XFERFILE examples
** using a command like:
**
**     fossil push http://bogus/ --httptrace
**
** The complete HTTP requests are stored in files named "http-request-N.txt".
** Find one of those requests, remove the HTTP header, and make other edits
** as necessary to generate an appropriate XFERFILE test case.  Then run:
**
**     fossil test-xfer xferfile.txt
**
** Options:
**    --host  HOSTNAME             Supply a server hostname used to populate
**                                 g.zBaseURL and similar.
*/
void cmd_test_xfer(void){
  const char *zHost;
  db_find_and_open_repository(0,0);
  zHost = find_option("host",0,1);
  verify_all_options();
  if( g.argc!=2 && g.argc!=3 ){
    usage("?MESSAGEFILE?");
  }
  if( zHost==0 ) zHost = "localhost:8080";
  g.zBaseURL = mprintf("http://%s", zHost);
  g.zHttpsURL = mprintf("https://%s", zHost);
  g.zTop = mprintf("");
  blob_zero(&g.cgiIn);
  blob_read_from_file(&g.cgiIn, g.argc==2 ? "-" : g.argv[2], ExtFILE);
  disableLogin = 1;
  page_xfer();
  fossil_print("%s", cgi_extract_content());
}

/*
** Format strings for progress reporting.
*/
static const char zLabelFormat[] = "%-10s %10s %10s %10s %10s\n";
static const char zValueFormat[] = "\r%-10s %10d %10d %10d %10d\n";
static const char zBriefFormat[] =
   "Round-trips: %d   Artifacts sent: %d  received: %d\r";

#if INTERFACE
/*
** Flag options for controlling client_sync()
*/
#define SYNC_PUSH           0x00001    /* push content client to server */
#define SYNC_PULL           0x00002    /* pull content server to client */
#define SYNC_CLONE          0x00004    /* clone the repository */
#define SYNC_PRIVATE        0x00008    /* Also transfer private content */
#define SYNC_VERBOSE        0x00010    /* Extra diagnostics */
#define SYNC_RESYNC         0x00020    /* --verily */
#define SYNC_FROMPARENT     0x00040    /* Pull from the parent project */
#define SYNC_UNVERSIONED    0x00100    /* Sync unversioned content */
#define SYNC_UV_REVERT      0x00200    /* Copy server unversioned to client */
#define SYNC_UV_TRACE       0x00400    /* Describe UV activities */
#define SYNC_UV_DRYRUN      0x00800    /* Do not actually exchange files */
#define SYNC_IFABLE         0x01000    /* Inability to sync is not fatal */
#define SYNC_CKIN_LOCK      0x02000    /* Lock the current check-in */
#define SYNC_NOHTTPCOMPRESS 0x04000    /* Do not compression HTTP messages */
#define SYNC_ALLURL         0x08000    /* The --all flag - sync to all URLs */
#define SYNC_SHARE_LINKS    0x10000    /* Request alternate repo links */
#define SYNC_XVERBOSE       0x20000    /* Extra verbose.  Network traffic */
#endif

/*
** Floating-point absolute value
*/
static double fossil_fabs(double x){
  return x>0.0 ? x : -x;
}

/*
** Sync to the host identified in g.url.name and g.url.path.  This
** routine is called by the client.
**
** Records are pushed to the server if pushFlag is true.  Records
** are pulled if pullFlag is true.  A full sync occurs if both are
** true.
*/
int client_sync(
  unsigned syncFlags,      /* Mask of SYNC_* flags */
  unsigned configRcvMask,  /* Receive these configuration items */
  unsigned configSendMask, /* Send these configuration items */
  const char *zAltPCode,   /* Alternative project code (usually NULL) */
  int *pnRcvd              /* Set to # received artifacts, if not NULL */
){
  int go = 1;             /* Loop until zero */
  int nCardSent = 0;      /* Number of cards sent */
  int nCardRcvd = 0;      /* Number of cards received */
  int nCycle = 0;         /* Number of round trips to the server */
  int size;               /* Size of a config value or uvfile */
  int origConfigRcvMask;  /* Original value of configRcvMask */
  int nFileRecv;          /* Number of files received */
  int mxPhantomReq = 200; /* Max number of phantoms to request per comm */
  const char *zCookie;    /* Server cookie */
  i64 nUncSent, nUncRcvd; /* Bytes sent and received (before compression) */
  i64 nSent, nRcvd;       /* Bytes sent and received (after compression) */
  int cloneSeqno = 1;     /* Sequence number for clones */
  Blob send;              /* Text we are sending to the server */
  Blob recv;              /* Reply we got back from the server */
  Xfer xfer;              /* Transfer data */
  int pctDone;            /* Percentage done with a message */
  int lastPctDone = -1;   /* Last displayed pctDone */
  double rArrivalTime;    /* Time at which a message arrived */
  const char *zSCode = db_get("server-code", "x");
  const char *zPCode = db_get("project-code", 0);
  int nErr = 0;           /* Number of errors */
  int nRoundtrip= 0;      /* Number of HTTP requests */
  int nArtifactSent = 0;  /* Total artifacts sent */
  int nArtifactRcvd = 0;  /* Total artifacts received */
  int nPriorArtifact = 0; /* Artifacts received on prior round-trips */
  const char *zOpType = 0;/* Push, Pull, Sync, Clone */
  double rSkew = 0.0;     /* Maximum time skew */
  int uvHashSent = 0;     /* The "pragma uv-hash" message has been sent */
  int uvDoPush = 0;       /* Generate uvfile messages to send to server */
  int uvPullOnly = 0;     /* 1: pull-only.  2: pull-only warning issued */
  int nUvGimmeSent = 0;   /* Number of uvgimme cards sent on this cycle */
  int nUvFileRcvd = 0;    /* Number of uvfile cards received on this cycle */
  sqlite3_int64 mtime;    /* Modification time on a UV file */
  int autopushFailed = 0; /* Autopush following commit failed if true */
  const char *zCkinLock;  /* Name of check-in to lock.  NULL for none */
  const char *zClientId;  /* A unique identifier for this check-out */
  unsigned int mHttpFlags;/* Flags for the http_exchange() subsystem */

  if( pnRcvd ) *pnRcvd = 0;
  if( db_get_boolean("dont-push", 0) ) syncFlags &= ~SYNC_PUSH;
  if( (syncFlags & (SYNC_PUSH|SYNC_PULL|SYNC_CLONE|SYNC_UNVERSIONED))==0
     && configRcvMask==0
     && configSendMask==0
  ){
    return 0;  /* Nothing to do */
  }

  /* Compute an appropriate project code.  zPCode is the project code
  ** for the local repository.  zAltPCode will usually be NULL, but might
  ** also be an alternative project code to expect on the server.  When
  ** zAltPCode is not NULL, that means we are doing a cross-project import -
  ** in other words, reading content from one project into a different
  ** project.
  */
  if( syncFlags & SYNC_FROMPARENT ){
    const char *zPX;
    configRcvMask = 0;
    configSendMask = 0;
    syncFlags &= ~(SYNC_PUSH);
    zPX = db_get("parent-project-code", 0);
    if( zPX==0 || db_get("parent-project-name",0)==0 ){
      fossil_fatal("there is no parent project: set the 'parent-project-code'"
                   " and 'parent-project-name' config parameters in order"
                   " to pull from a parent project");
    }
    if( zPX ){
      zAltPCode = zPX;
    }
  }
  if( zAltPCode!=0 && zPCode!=0 && sqlite3_stricmp(zPCode, zAltPCode)==0 ){
    zAltPCode = 0;
  }

  transport_stats(0, 0, 1);
  socket_global_init();
  memset(&xfer, 0, sizeof(xfer));
  xfer.pIn = &recv;
  xfer.pOut = &send;
  xfer.mxSend = db_get_int("max-upload", 250000);
  xfer.maxTime = -1;
  xfer.remoteVersion = RELEASE_VERSION_NUMBER;
  if( syncFlags & SYNC_PRIVATE ){
    g.perm.Private = 1;
    xfer.syncPrivate = 1;
  }

  blobarray_zero(xfer.aToken, count(xfer.aToken));
  blob_zero(&send);
  blob_zero(&recv);
  blob_zero(&xfer.err);
  blob_zero(&xfer.line);
  origConfigRcvMask = 0;
  nUncSent = nUncRcvd = 0;

  /* Send the send-private pragma if we are trying to sync private data */
  if( syncFlags & SYNC_PRIVATE ){
    blob_append(&send, "pragma send-private\n", -1);
  }

  /* Figure out which check-in to lock */
  if( syncFlags & SYNC_CKIN_LOCK ){
    int vid = db_lget_int("checkout",0);
    zCkinLock = db_text(0, "SELECT uuid FROM blob WHERE rid=%d", vid);
  }else{
    zCkinLock = 0;
  }
  zClientId = g.localOpen ? db_lget("client-id", 0) : 0;

  /* When syncing unversioned files, create a TEMP table in which to store
  ** the names of files that need to be sent from client to server.
  **
  ** The initial assumption is that all unversioned files need to be sent
  ** to the other side.  But "uvigot" cards received back from the remote
  ** side will normally cause many of these entries to be removed since they
  ** do not really need to be sent.
  */
  if( (syncFlags & (SYNC_UNVERSIONED|SYNC_CLONE))!=0 ){
    unversioned_schema();
    db_multi_exec(
       "CREATE TEMP TABLE IF NOT EXISTS uv_tosend("
       "  name TEXT PRIMARY KEY,"  /* Name of file to send client->server */
       "  mtimeOnly BOOLEAN"       /* True to only send mtime, not content */
       ") WITHOUT ROWID;"
       "REPLACE INTO uv_tosend(name,mtimeOnly)"
       "  SELECT name, 0 FROM unversioned WHERE hash IS NOT NULL;"
    );
  }

  /*
  ** The request from the client always begin with a clone, pull,
  ** or push message.
  */
  blob_appendf(&send, "pragma client-version %d %d %d\n",
               RELEASE_VERSION_NUMBER, MANIFEST_NUMERIC_DATE,
               MANIFEST_NUMERIC_TIME);
  if( syncFlags & SYNC_CLONE ){
    blob_appendf(&send, "clone 3 %d\n", cloneSeqno);
    syncFlags &= ~(SYNC_PUSH|SYNC_PULL);
    nCardSent++;
    /* TBD: Request all transferable configuration values */
    content_enable_dephantomize(0);
    zOpType = "Clone";
  }else if( syncFlags & SYNC_PULL ){
    blob_appendf(&send, "pull %s %s\n", zSCode,
                 zAltPCode ? zAltPCode : zPCode);
    nCardSent++;
    zOpType = (syncFlags & SYNC_PUSH)?"Sync":"Pull";
    if( (syncFlags & SYNC_RESYNC)!=0 && nCycle<2 ){
      blob_appendf(&send, "pragma send-catalog\n");
      nCardSent++;
    }
  }
  if( syncFlags & SYNC_PUSH ){
    blob_appendf(&send, "push %s %s\n", zSCode, zPCode);
    nCardSent++;
    if( (syncFlags & SYNC_PULL)==0 ) zOpType = "Push";
    if( (syncFlags & SYNC_RESYNC)!=0 ) xfer.resync = 0x7fffffff;
  }
  if( syncFlags & SYNC_VERBOSE ){
    fossil_print(zLabelFormat /*works-like:"%s%s%s%s%d"*/,
                 "", "Bytes", "Cards", "Artifacts", "Deltas");
  }

  /* Send the client-url pragma on the first cycle if the client has
  ** a known public url.
  */
  if( zAltPCode==0 ){
    const char *zSelfUrl = public_url();
    if( zSelfUrl ){
      blob_appendf(&send, "pragma client-url %s\n", zSelfUrl);
    }
  }

  /* Request URLs of alternative repositories
  */
  if( zAltPCode==0 && (syncFlags & SYNC_SHARE_LINKS)!=0 ){
    blob_appendf(&send, "pragma req-links\n");
  }

  while( go ){
    int newPhantom = 0;
    char *zRandomness;
    db_begin_transaction();
    db_record_repository_filename(0);
    db_multi_exec(
      "CREATE TEMP TABLE peerhave(uuid TEXT PRIMARY KEY) WITHOUT ROWID;"
      "CREATE TEMP TABLE peerneed(uuid TEXT PRIMARY KEY) WITHOUT ROWID;"
    );
    manifest_crosslink_begin();


    /* Client sends the most recently received cookie back to the server.
    ** Let the server figure out if this is a cookie that it cares about.
    */
    zCookie = db_get("cookie", 0);
    if( zCookie ){
      blob_appendf(&send, "cookie %s\n", zCookie);
    }

    /* Client sends gimme cards for phantoms
    */
    if( (syncFlags & SYNC_PULL)!=0
     || ((syncFlags & SYNC_CLONE)!=0 && cloneSeqno==1)
    ){
      request_partials(&xfer, mxPhantomReq);
    }
    if( syncFlags & SYNC_PUSH ){
      // frybox暂时不支持unsent优化
      // send_unsent(&xfer);
      nCardSent += send_root(&xfer);
      if( syncFlags & SYNC_PRIVATE ) send_private(&xfer);
    }

    /* Client sends configuration parameter requests.  On a clone, delay sending
    ** this until the second cycle since the login card might fail on
    ** the first cycle.
    */
    if( configRcvMask && ((syncFlags & SYNC_CLONE)==0 || nCycle>0) ){
      const char *zName;
      if( zOpType==0 ) zOpType = "Pull";
      zName = configure_first_name(configRcvMask);
      while( zName ){
        blob_appendf(&send, "reqconfig %s\n", zName);
        zName = configure_next_name(configRcvMask);
        nCardSent++;
      }
      origConfigRcvMask = configRcvMask;
      configRcvMask = 0;
    }

    /* Client sends a request to sync unversioned files.
    ** On a clone, delay sending this until the second cycle since
    ** the login card might fail on the first cycle.
    */
    if( (syncFlags & SYNC_UNVERSIONED)!=0
     && ((syncFlags & SYNC_CLONE)==0 || nCycle>0)
     && !uvHashSent
    ){
      blob_appendf(&send, "pragma uv-hash %s\n", unversioned_content_hash(0));
      nCardSent++;
      uvHashSent = 1;
    }

    /* On a "fossil config push", the client send configuration parameters
    ** being pushed up to the server */
    if( configSendMask ){
      if( zOpType==0 ) zOpType = "Push";
      nCardSent += configure_send_group(xfer.pOut, configSendMask, 0);
      configSendMask = 0;
    }

    /* Send unversioned files present here on the client but missing or
    ** obsolete on the server.
    **
    ** Or, if the SYNC_UV_REVERT flag is set, delete the local unversioned
    ** files that do not exist on the server.
    **
    ** This happens on the second exchange, since we do not know what files
    ** need to be sent until after the uvigot cards from the first exchange
    ** have been processed.
    */
    if( uvDoPush ){
      assert( (syncFlags & SYNC_UNVERSIONED)!=0 );
      if( syncFlags & SYNC_UV_DRYRUN ){
        uvDoPush = 0;
      }else if( syncFlags & SYNC_UV_REVERT ){
        db_multi_exec(
          "DELETE FROM unversioned"
          " WHERE name IN (SELECT name FROM uv_tosend);"
          "DELETE FROM uv_tosend;"
        );
        uvDoPush = 0;
      }else{
        Stmt uvq;
        int rc = SQLITE_OK;
        db_prepare(&uvq, "SELECT name, mtimeOnly FROM uv_tosend");
        while( (rc = db_step(&uvq))==SQLITE_ROW ){
          const char *zName = db_column_text(&uvq, 0);
          send_unversioned_file(&xfer, zName, db_column_int(&uvq,1));
          nCardSent++;
          nArtifactSent++;
          db_multi_exec("DELETE FROM uv_tosend WHERE name=%Q", zName);
          if( syncFlags & SYNC_VERBOSE ){
            fossil_print("\rUnversioned-file sent: %s\n", zName);
          }
          if( (int)blob_size(xfer.pOut)>xfer.mxSend ) break;
        }
        db_finalize(&uvq);
        if( rc==SQLITE_DONE ) uvDoPush = 0;
      }
    }

    /* Lock the current check-out */
    if( zCkinLock ){
      if( zClientId==0 ){
        zClientId = db_text(0, "SELECT lower(hex(randomblob(20)))");
        db_lset("client-id", zClientId);
      }
      blob_appendf(&send, "pragma ci-lock %s %s\n", zCkinLock, zClientId);
      zCkinLock = 0;
    }else if( zClientId ){
      blob_appendf(&send, "pragma ci-unlock %s\n", zClientId);
    }

    /* Append randomness to the end of the uplink message.  This makes all
    ** messages unique so that that the login-card nonce will always
    ** be unique.
    */
    zRandomness = db_text(0, "SELECT hex(randomblob(20))");
    blob_appendf(&send, "# %s\n", zRandomness);
    free(zRandomness);

    if( (syncFlags & SYNC_VERBOSE)!=0
     && (syncFlags & SYNC_XVERBOSE)==0
    ){
      fossil_print("waiting for server...");
    }
    fflush(stdout);
    /* Exchange messages with the server */
    if( (syncFlags & SYNC_CLONE)!=0 && nCycle==0 ){
      /* Do not send a login card on the first round-trip of a clone */
      mHttpFlags = 0;
    }else{
      mHttpFlags = HTTP_USE_LOGIN;
    }
    if( syncFlags & SYNC_NOHTTPCOMPRESS ){
      mHttpFlags |= HTTP_NOCOMPRESS;
    }
    if( syncFlags & SYNC_XVERBOSE ){
      mHttpFlags |= HTTP_VERBOSE;
    }

    /* Do the round-trip to the server */
    if( http_exchange(&send, &recv, mHttpFlags, MAX_REDIRECTS, 0) ){
      nErr++;
      go = 2;
      break;
    }

    /* Remember the URL of the sync target in the config file on the
    ** first successful round-trip */
    if( nCycle==0 && db_is_writeable("repository") ){
      xfer_syncwith(g.url.canonical, 0);
    }

    /* Output current stats */
    if( syncFlags & SYNC_VERBOSE ){
      fossil_print(zValueFormat /*works-like:"%s%d%d%d%d"*/, "Sent:",
                   blob_size(&send), nCardSent+xfer.nGimmeSent+xfer.nIGotSent,
                   xfer.nFileSent, xfer.nDeltaSent);
    }else{
      nRoundtrip++;
      nArtifactSent += xfer.nFileSent + xfer.nDeltaSent;
      fossil_print(zBriefFormat /*works-like:"%d%d%d"*/,
                   nRoundtrip, nArtifactSent, nArtifactRcvd);
    }
    nCardSent = 0;
    nCardRcvd = 0;
    xfer.nFileSent = 0;
    xfer.nDeltaSent = 0;
    xfer.nGimmeSent = 0;
    xfer.nIGotSent = 0;
    xfer.nPrivIGot = 0;

    lastPctDone = -1;
    nUncSent += blob_size(&send);
    blob_reset(&send);
    blob_appendf(&send, "pragma client-version %d %d %d\n",
                 RELEASE_VERSION_NUMBER, MANIFEST_NUMERIC_DATE,
                 MANIFEST_NUMERIC_TIME);
    rArrivalTime = db_double(0.0, "SELECT julianday('now')");

    /* Send the send-private pragma if we are trying to sync private data */
    if( syncFlags & SYNC_PRIVATE ){
      blob_append(&send, "pragma send-private\n", -1);
    }

    /* Begin constructing the next message (which might never be
    ** sent) by beginning with the pull or push cards
    */
    if( syncFlags & SYNC_PULL ){
      blob_appendf(&send, "pull %s %s\n", zSCode,
                   zAltPCode ? zAltPCode : zPCode);
      nCardSent++;
    }
    if( syncFlags & SYNC_PUSH ){
      blob_appendf(&send, "push %s %s\n", zSCode, zPCode);
      nCardSent++;
    }
    go = 0;
    nUvGimmeSent = 0;
    nUvFileRcvd = 0;
    nPriorArtifact = nArtifactRcvd;

    /* Process the reply that came back from the server */
    while( blob_line(&recv, &xfer.line) ){
      if( blob_buffer(&xfer.line)[0]=='#' ){
        const char *zLine = blob_buffer(&xfer.line);
        if( memcmp(zLine, "# timestamp ", 12)==0 ){
          char zTime[20];
          double rDiff;
          sqlite3_snprintf(sizeof(zTime), zTime, "%.19s", &zLine[12]);
          rDiff = db_double(9e99, "SELECT julianday('%q') - %.17g",
                            zTime, rArrivalTime);
          if( rDiff>9e98 || rDiff<-9e98 ) rDiff = 0.0;
          if( rDiff*24.0*3600.0 >= -(blob_size(&recv)/5000.0 + 20) ){
            rDiff = 0.0;
          }
          if( fossil_fabs(rDiff)>fossil_fabs(rSkew) ) rSkew = rDiff;
        }
        nCardRcvd++;
        continue;
      }
      xfer.nToken = blob_tokenize(&xfer.line, xfer.aToken, count(xfer.aToken));
      nCardRcvd++;
      if( (syncFlags & SYNC_VERBOSE)!=0 && recv.nUsed>0 ){
        pctDone = (recv.iCursor*100)/recv.nUsed;
        if( pctDone!=lastPctDone ){
          fossil_print("\rprocessed: %d%%         ", pctDone);
          lastPctDone = pctDone;
          fflush(stdout);
        }
      }

      /*   file HASH SIZE \n CONTENT
      **   file HASH DELTASRC SIZE \n CONTENT
      **
      ** Client receives a file transmitted from the server.
      */
      if( blob_eq(&xfer.aToken[0],"file") ){
        xfer_accept_file(&xfer, (syncFlags & SYNC_CLONE)!=0, 0, 0);
        nArtifactRcvd++;
      }else

      /*   cfile HASH USIZE CSIZE \n CONTENT
      **   cfile HASH DELTASRC USIZE CSIZE \n CONTENT
      **
      ** Client receives a compressed file transmitted from the server.
      */
      if( blob_eq(&xfer.aToken[0],"cfile") ){
        xfer_accept_compressed_file(&xfer, 0, 0);
        nArtifactRcvd++;
      }else

      /*   uvfile NAME MTIME HASH SIZE FLAGS \n CONTENT
      **
      ** Client accepts an unversioned file from the server.
      */
      if( blob_eq(&xfer.aToken[0], "uvfile") ){
        xfer_accept_unversioned_file(&xfer, 1);
        nArtifactRcvd++;
        nUvFileRcvd++;
        if( syncFlags & SYNC_VERBOSE ){
          fossil_print("\rUnversioned-file received: %s\n",
                       blob_str(&xfer.aToken[1]));
        }
      }else

      /*   gimme HASH
      **
      ** Client receives an artifact request from the server.
      ** If the file is a manifest, assume that the server will also want
      ** to know all of the content artifacts associated with the manifest
      ** and send those too.
      */
      if( blob_eq(&xfer.aToken[0], "gimme")
       && xfer.nToken==2
       && blob_is_hname(&xfer.aToken[1])
      ){
        remote_unk(&xfer.aToken[1]);
        if( syncFlags & SYNC_PUSH ){
          int rid = nid_from_uuid(&xfer.aToken[1], 0, 0);
          if( rid ) send_file(&xfer, rid, &xfer.aToken[1], 0);
        }
      }else

      /*   igot HASH  ?PRIVATEFLAG?
      **
      ** Server announces that it has a particular file.  If this is
      ** not a file that we have and we are pulling, then create a
      ** phantom to cause this file to be requested on the next cycle.
      ** Always remember that the server has this file so that we do
      ** not transmit it by accident.
      **
      ** If the PRIVATE argument exists and is 1, then the file is
      ** private.  Pretend it does not exists if we are not pulling
      ** private files.
      */
      if( xfer.nToken>=2
       && blob_eq(&xfer.aToken[0], "igot")
       && blob_is_hname(&xfer.aToken[1])
      ){
        int rid;
        int isPriv = xfer.nToken>=3 && blob_eq(&xfer.aToken[2],"1");
        rid = nid_from_uuid(&xfer.aToken[1], 0, 0);
        if( rid>0 ){
          if( isPriv ){
            content_make_private(rid);
          }else{
            content_make_public(rid);
          }
        }else if( isPriv && !g.perm.Private ){
          /* ignore private files */
        }else if( (syncFlags & (SYNC_PULL|SYNC_CLONE))!=0 ){
          rid = content_new(blob_str(&xfer.aToken[1]), isPriv);
          if( rid ) newPhantom = 1;
        }
        peer_have(rid);
      }else

      /*   uvigot NAME MTIME HASH SIZE
      **
      ** Server announces that it has a particular unversioned file.  The
      ** server will only send this card if the client had previously sent
      ** a "pragma uv-hash" card with a hash that does not match.
      **
      ** If the identified file needs to be transferred, then setup for the
      ** transfer.  Generate a "uvgimme" card in the reply if the server
      ** version is newer than the client.  Generate a "uvfile" card if
      ** the client version is newer than the server.  If HASH is "-"
      ** (indicating that the file has been deleted) and MTIME is newer,
      ** then do the deletion.
      */
      if( xfer.nToken==5
       && blob_eq(&xfer.aToken[0], "uvigot")
       && blob_is_filename(&xfer.aToken[1])
       && blob_is_int64(&xfer.aToken[2], &mtime)
       && blob_is_int(&xfer.aToken[4], &size)
       && (blob_eq(&xfer.aToken[3],"-") || blob_is_hname(&xfer.aToken[3]))
      ){
        const char *zName = blob_str(&xfer.aToken[1]);
        const char *zHash = blob_str(&xfer.aToken[3]);
        int iStatus;
        iStatus = unversioned_status(zName, mtime, zHash);
        if( (syncFlags & SYNC_UV_REVERT)!=0 ){
          if( iStatus==4 ) iStatus = 2;
          if( iStatus==5 ) iStatus = 1;
        }
        if( syncFlags & (SYNC_UV_TRACE|SYNC_UV_DRYRUN) ){
          const char *zMsg = 0;
          switch( iStatus ){
            case 0:
            case 1: zMsg = "UV-PULL";             break;
            case 2: zMsg = "UV-PULL-MTIME-ONLY";  break;
            case 4: zMsg = "UV-PUSH-MTIME-ONLY";  break;
            case 5: zMsg = "UV-PUSH";             break;
          }
          if( zMsg ) fossil_print("\r%s: %s\n", zMsg, zName);
          if( syncFlags & SYNC_UV_DRYRUN ){
            iStatus = 99;  /* Prevent any changes or reply messages */
          }
        }
        if( iStatus<=1 ){
          if( zHash[0]!='-' ){
            blob_appendf(xfer.pOut, "uvgimme %s\n", zName);
            nCardSent++;
            nUvGimmeSent++;
            db_multi_exec("DELETE FROM unversioned WHERE name=%Q", zName);
          }else if( iStatus==1 ){
            db_multi_exec(
               "UPDATE unversioned"
               "   SET mtime=%lld, hash=NULL, sz=0, encoding=0, content=NULL"
               " WHERE name=%Q", mtime, zName
            );
            db_unset("uv-hash", 0);
          }
        }else if( iStatus==2 ){
          db_multi_exec(
            "UPDATE unversioned SET mtime=%lld WHERE name=%Q", mtime, zName
          );
          db_unset("uv-hash", 0);
        }
        if( iStatus>=4 && uvPullOnly==1 ){
          fossil_warning(
            "Warning: uv-pull-only                                       \n"
            "         Unable to push unversioned content because you lack\n"
            "         sufficient permission on the server\n"
          );
          uvPullOnly = 2;
        }
        if( iStatus<=3 || uvPullOnly ){
          db_multi_exec("DELETE FROM uv_tosend WHERE name=%Q", zName);
        }else if( iStatus==4 ){
          db_multi_exec("UPDATE uv_tosend SET mtimeOnly=1 WHERE name=%Q",zName);
        }else if( iStatus==5 ){
          db_multi_exec("REPLACE INTO uv_tosend(name,mtimeOnly) VALUES(%Q,0)",
                        zName);
        }
      }else

      /*   push  SERVERCODE  PRODUCTCODE
      **
      ** Should only happen in response to a clone.  This message tells
      ** the client what product code to use for the new database.
      */
      if( blob_eq(&xfer.aToken[0],"push")
       && xfer.nToken==3
       && (syncFlags & SYNC_CLONE)!=0
       && blob_is_hname(&xfer.aToken[2])
      ){
        if( zPCode==0 ){
          zPCode = mprintf("%b", &xfer.aToken[2]);
          db_set("project-code", zPCode, 0);
        }
        if( cloneSeqno>0 ) blob_appendf(&send, "clone 3 %d\n", cloneSeqno);
        nCardSent++;
      }else

      /*   config NAME SIZE \n CONTENT
      **
      ** Client receive a configuration value from the server.
      **
      ** The received configuration setting is silently ignored if it was
      ** not requested by a prior "reqconfig" sent from client to server.
      */
      if( blob_eq(&xfer.aToken[0],"config") && xfer.nToken==3
          && blob_is_int(&xfer.aToken[2], &size) ){
        const char *zName = blob_str(&xfer.aToken[1]);
        Blob content;
        blob_zero(&content);
        blob_extract(xfer.pIn, size, &content);
        g.perm.Admin = g.perm.RdAddr = 1;
        configure_receive(zName, &content, origConfigRcvMask);
        nCardRcvd++;
        nArtifactRcvd++;
        blob_reset(&content);
        blob_seek(xfer.pIn, 1, BLOB_SEEK_CUR);
      }else


      /*    cookie TEXT
      **
      ** The client reserves a cookie from the server.  The client
      ** should remember this cookie and send it back to the server
      ** in its next query.
      **
      ** Each cookie received overwrites the prior cookie from the
      ** same server.
      */
      if( blob_eq(&xfer.aToken[0], "cookie") && xfer.nToken==2 ){
        db_set("cookie", blob_str(&xfer.aToken[1]), 0);
      }else


      /*    private
      **
      ** The server tells the client that the next "file" or "cfile" will
      ** contain private content.
      */
      if( blob_eq(&xfer.aToken[0], "private") ){
        xfer.nextIsPrivate = 1;
      }else


      /*    clone_seqno N
      **
      ** When doing a clone, the server tries to send all of its artifacts
      ** in sequence.  This card indicates the sequence number of the next
      ** blob that needs to be sent.  If N<=0 that indicates that all blobs
      ** have been sent.
      */
      if( blob_eq(&xfer.aToken[0], "clone_seqno") && xfer.nToken==2 ){
        blob_is_int(&xfer.aToken[1], &cloneSeqno);
      }else

      /*   message MESSAGE
      **
      ** A message is received from the server.  Print it.
      ** Similar to "error" but does not stop processing.
      **
      ** If the "login failed" message is seen, clear the sync password prior
      ** to the next cycle.
      */
      if( blob_eq(&xfer.aToken[0],"message") && xfer.nToken==2 ){
        char *zMsg = blob_terminate(&xfer.aToken[1]);
        defossilize(zMsg);
        if( (syncFlags & SYNC_PUSH) && zMsg
            && sqlite3_strglob("pull only *", zMsg)==0 ){
          syncFlags &= ~SYNC_PUSH;
          zMsg = 0;
        }
        if( zMsg && zMsg[0] ){
          fossil_force_newline();
          fossil_print("Server says: %s\n", zMsg);
        }
      }else

      /*    pragma NAME VALUE...
      **
      ** The server can send pragmas to try to convey meta-information to
      ** the client.  These are informational only.  Unknown pragmas are
      ** silently ignored.
      */
      if( blob_eq(&xfer.aToken[0], "pragma") && xfer.nToken>=2 ){
        /*   pragma server-version VERSION ?DATE? ?TIME?
        **
        ** The server announces to the server what version of Fossil it
        ** is running.  The DATE and TIME are a pure numeric ISO8601 time
        ** for the specific check-in of the client.
        */
        if( xfer.nToken>=3 && blob_eq(&xfer.aToken[1], "server-version") ){
          xfer.remoteVersion = atoi(blob_str(&xfer.aToken[2]));
          if( xfer.nToken>=5 ){
            xfer.remoteDate = atoi(blob_str(&xfer.aToken[3]));
            xfer.remoteTime = atoi(blob_str(&xfer.aToken[4]));
          }
        }

        /*   pragma uv-pull-only
        **   pragma uv-push-ok
        **
        ** If the server is unwilling to accept new unversioned content (because
        ** this client lacks the necessary permissions) then it sends a
        ** "uv-pull-only" pragma so that the client will know not to waste
        ** bandwidth trying to upload unversioned content.  If the server
        ** does accept new unversioned content, it sends "uv-push-ok".
        */
        else if( syncFlags & SYNC_UNVERSIONED ){
          if( blob_eq(&xfer.aToken[1], "uv-pull-only") ){
            uvPullOnly = 1;
            if( syncFlags & SYNC_UV_REVERT ) uvDoPush = 1;
          }else if( blob_eq(&xfer.aToken[1], "uv-push-ok") ){
            uvDoPush = 1;
          }
        }

        /*    pragma ci-lock-fail  USER-HOLDING-LOCK  LOCK-TIME
        **
        ** The server generates this message when a "pragma ci-lock"
        ** is attempted on a check-in for which there is an existing
        ** lock.  USER-HOLDING-LOCK is the name of the user who originated
        ** the lock, and LOCK-TIME is the timestamp (seconds since 1970)
        ** when the lock was taken.
        */
        else if( blob_eq(&xfer.aToken[1], "ci-lock-fail") && xfer.nToken==4 ){
          char *zUser = blob_terminate(&xfer.aToken[2]);
          sqlite3_int64 mtime, iNow;
          defossilize(zUser);
          iNow = time(NULL);
          if( blob_is_int64(&xfer.aToken[3], &mtime) && iNow>mtime ){
            iNow = time(NULL);
            fossil_print("\nParent check-in locked by %s %s ago\n",
               zUser, human_readable_age((iNow+1-mtime)/86400.0));
          }else{
            fossil_print("\nParent check-in locked by %s\n", zUser);
          }
          g.ckinLockFail = fossil_strdup(zUser);
        }

        /*    pragma avoid-delta-manifests
        **
        ** Discourage the use of delta manifests.  The remote side sends
        ** this pragma when its forbid-delta-manifests setting is true.
        */
        else if( blob_eq(&xfer.aToken[1], "avoid-delta-manifests") ){
          g.bAvoidDeltaManifests = 1;
        }

        /*    pragma link URL ARG MTIME
        **
        ** The server has sent the URL for a link to another repository.
        ** Record this as a link:URL entry in the config table.
        */
        else if( blob_eq(&xfer.aToken[1], "link")
              && xfer.nToken==5
              && (syncFlags & SYNC_SHARE_LINKS)!=0
        ){
          UrlData x;
          char *zUrl = blob_str(&xfer.aToken[2]);
          char *zArg = blob_str(&xfer.aToken[3]);
          i64 iTime = strtoll(blob_str(&xfer.aToken[4]),0,0);
          memset(&x, 0, sizeof(x));
          defossilize(zUrl);
          defossilize(zArg);
          url_parse_local(zUrl, URL_OMIT_USER, &x);
          if( x.protocol
           && strncmp(x.protocol,"http",4)==0
           && iTime>0
          ){
            db_unprotect(PROTECT_CONFIG);
            db_multi_exec(
              "INSERT INTO config(name,value,mtime)\n"
              " VALUES('link:%q',%Q,%lld)\n"
              " ON CONFLICT DO UPDATE\n"
              "   SET value=excluded.value, mtime=excluded.mtime\n"
              "   WHERE mtime<excluded.mtime;",
              zUrl, zArg, iTime
            );
            db_protect_pop();
          }
          url_unparse(&x);
        }

      }else

      /*   error MESSAGE
      **
      ** The server is reporting an error.  The client will abandon
      ** the sync session.
      **
      ** Except, when cloning we will sometimes get an error on the
      ** first message exchange because the project-code is unknown
      ** and so the login card on the request was invalid.  The project-code
      ** is returned in the reply before the error card, so second and
      ** subsequent messages should be OK.  Nevertheless, we need to ignore
      ** the error card on the first message of a clone.
      **
      ** Also ignore "not authorized to write" errors if this is an
      ** autopush following a commit.
      */
      if( blob_eq(&xfer.aToken[0],"error") && xfer.nToken==2 ){
        char *zMsg = blob_terminate(&xfer.aToken[1]);
        defossilize(zMsg);
        if( (syncFlags & SYNC_IFABLE)!=0
         && sqlite3_strlike("%not authorized to write%",zMsg,0)==0 ){
          autopushFailed = 1;
          nErr++;
        }else if( (syncFlags & SYNC_CLONE)==0 || nCycle>0 ){
          fossil_force_newline();
          fossil_print("Error: %s\n", zMsg);
          blob_appendf(&xfer.err, "server says: %s\n", zMsg);
          nErr++;
          break;
        }
      }else

      /* Unknown message */
      if( xfer.nToken>0 ){
        if( blob_str(&xfer.aToken[0])[0]=='<' ){
          fossil_warning(
            "server replies with HTML instead of fossil sync protocol:\n%b",
            &recv
          );
          nErr++;
          break;
        }
        blob_appendf(&xfer.err, "unknown command: [%b]\n", &xfer.aToken[0]);
      }

      if( blob_size(&xfer.err) ){
        fossil_force_newline();
        fossil_warning("%b", &xfer.err);
        nErr++;
        break;
      }
      blobarray_reset(xfer.aToken, xfer.nToken);
      blob_reset(&xfer.line);
    }
    origConfigRcvMask = 0;
    if( nCardRcvd>0 && (syncFlags & SYNC_VERBOSE) ){
      fossil_print(zValueFormat /*works-like:"%s%d%d%d%d"*/, "Received:",
                   blob_size(&recv), nCardRcvd,
                   xfer.nFileRcvd, xfer.nDeltaRcvd + xfer.nDanglingFile);
    }else{
      fossil_print(zBriefFormat /*works-like:"%d%d%d"*/,
                   nRoundtrip, nArtifactSent, nArtifactRcvd);
    }
    nUncRcvd += blob_size(&recv);
    blob_reset(&recv);
    nCycle++;

    /* Set go to 1 if we need to continue the sync/push/pull/clone for
    ** another round.  Set go to 0 if it is time to quit. */
    nFileRecv = xfer.nFileRcvd + xfer.nDeltaRcvd + xfer.nDanglingFile;
    if( (nFileRecv>0 || newPhantom) && db_exists("SELECT 1 FROM phantom") ){
      go = 1;
      mxPhantomReq = nFileRecv*2;
      if( mxPhantomReq<200 ) mxPhantomReq = 200;
    }else if( xfer.nFileSent+xfer.nDeltaSent>0 || uvDoPush ){
      /* Go another round if files are queued to send */
      go = 1;
    }else if( xfer.nPrivIGot>0 && nCycle==1 ){
      go = 1;
    }else if( nUvGimmeSent>0 && (nUvFileRcvd>0 || nCycle<3) ){
      /* Continue looping as long as new uvfile cards are being received
      ** and uvgimme cards are being sent. */
      go = 1;
    }else if( (syncFlags & SYNC_CLONE)!=0 ){
      if( nCycle==1 ){
        go = 1;   /* go at least two rounds on a clone */
      }else if( nFileRecv>0 ){
        go = 1;
      }else if( cloneSeqno>0 && nArtifactRcvd>nPriorArtifact ){
        /* Continue the clone until we see the clone_seqno 0" card or
        ** until we stop receiving artifacts */
        go = 1;
      }
    }

    nCardRcvd = 0;
    xfer.nFileRcvd = 0;
    xfer.nDeltaRcvd = 0;
    xfer.nDanglingFile = 0;
    db_multi_exec("DROP TABLE peerhave; DROP TABLE peerneed;");
    if( go ){
      manifest_crosslink_end(MC_PERMIT_HOOKS);
    }else{
      manifest_crosslink_end(MC_PERMIT_HOOKS);
      content_enable_dephantomize(1);
    }
    db_end_transaction(0);
  };
  transport_stats(&nSent, &nRcvd, 1);
  if( pnRcvd ) *pnRcvd = nArtifactRcvd;
  if( (rSkew*24.0*3600.0) > 10.0 ){
     fossil_warning("*** time skew *** server is fast by %s",
                    db_timespan_name(rSkew));
     g.clockSkewSeen = 1;
  }else if( rSkew*24.0*3600.0 < -10.0 ){
     fossil_warning("*** time skew *** server is slow by %s",
                    db_timespan_name(-rSkew));
     g.clockSkewSeen = 1;
  }

  fossil_force_newline();
  if( g.zHttpCmd==0 ){
    if( syncFlags & SYNC_VERBOSE ){
      fossil_print(
        "%s done, wire bytes sent: %lld  received: %lld  remote: %s%s\n",
        zOpType, nSent, nRcvd,
        (g.url.name && g.url.name[0]!='\0') ? g.url.name : "",
        (g.zIpAddr && g.zIpAddr[0]!='\0'
          && fossil_strcmp(g.zIpAddr, g.url.name))
          ? mprintf(" (%s)", g.zIpAddr) : "");
    }else{
      fossil_print(
        "%s done, wire bytes sent: %lld  received: %lld  remote: %s\n",
          zOpType, nSent, nRcvd, g.zIpAddr);
    }
  }
  if( syncFlags & SYNC_VERBOSE ){
    fossil_print(
      "Uncompressed payload sent: %lld  received: %lld\n", nUncSent, nUncRcvd);
  }
  blob_reset(&send);
  blob_reset(&recv);
  transport_close(&g.url);
  transport_global_shutdown(&g.url);
  if( nErr && go==2 ){
    db_multi_exec("DROP TABLE peerhave; DROP TABLE peerneed;");
    manifest_crosslink_end(MC_PERMIT_HOOKS);
    content_enable_dephantomize(1);
    db_end_transaction(0);
  }
  if( nErr && autopushFailed ){
    fossil_warning(
      "Warning: The check-in was successful and is saved locally but you\n"
      "         are not authorized to push the changes back to the server\n"
      "         at %s",
      g.url.canonical
    );
    nErr--;
  }
  if( (syncFlags & SYNC_CLONE)==0 && g.rcvid && fossil_any_has_fork(g.rcvid) ){
    fossil_warning("***** WARNING: a fork has occurred *****\n"
                   "use \"fossil leaves -multiple\" for more details.");
  }
  return nErr;
}
