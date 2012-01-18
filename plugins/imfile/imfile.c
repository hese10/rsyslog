/* imfile.c
 * 
 * This is the input module for reading text file data. A text file is a
 * non-binary file who's lines are delemited by the \n character.
 *
 * Work originally begun on 2008-02-01 by Rainer Gerhards
 *
 * Copyright 2008-2012 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h" /* this is for autotools and always must be the first include */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>		/* do NOT remove: will soon be done by the module generation macros */
#ifdef HAVE_SYS_STAT_H
#	include <sys/stat.h>
#endif
#include "rsyslog.h"		/* error codes etc... */
#include "dirty.h"
#include "cfsysline.h"		/* access to config file objects */
#include "module-template.h"	/* generic module interface code - very important, read it! */
#include "srUtils.h"		/* some utility functions */
#include "msg.h"
#include "stream.h"
#include "errmsg.h"
#include "glbl.h"
#include "datetime.h"
#include "unicode-helper.h"
#include "prop.h"
#include "stringbuf.h"
#include "ruleset.h"

MODULE_TYPE_INPUT	/* must be present for input modules, do not remove */
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("imfile")

/* defines */

/* Module static data */
DEF_IMOD_STATIC_DATA	/* must be present, starts static data */
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)
DEFobjCurrIf(datetime)
DEFobjCurrIf(strm)
DEFobjCurrIf(prop)
DEFobjCurrIf(ruleset)

#define NUM_MULTISUB 1024 /* max number of submits -- TODO: make configurable */
typedef struct fileInfo_s {
	uchar *pszFileName;
	uchar *pszTag;
	size_t lenTag;
	uchar *pszStateFile; /* file in which state between runs is to be stored */
	int iFacility;
	int iSeverity;
	int maxLinesAtOnce;
	int nRecords; /**< How many records did we process before persisting the stream? */
	int iPersistStateInterval; /**< how often should state be persisted? (0=on close only) */
	strm_t *pStrm;	/* its stream (NULL if not assigned) */
	int readMode;	/* which mode to use in ReadMulteLine call? */
	ruleset_t *pRuleset;	/* ruleset to bind listener to (use system default if unspecified) */
	multi_submit_t multiSub;
} fileInfo_t;


/* forward definitions */
static rsRetVal persistStrmState(fileInfo_t *pInfo);

/* config variables */
struct modConfData_s {
	EMPTY_STRUCT;
};

static uchar *pszFileName = NULL;
static uchar *pszFileTag = NULL;
static uchar *pszStateFile = NULL;
static int iPollInterval = 10;	/* number of seconds to sleep when there was no file activity */
static int iPersistStateInterval = 0;	/* how often if state file to be persisted? (default 0->never) */
static int iFacility = 128; /* local0 */
static int iSeverity = 5;  /* notice, as of rfc 3164 */
static int readMode = 0;  /* mode to use for ReadMultiLine call */
static int maxLinesAtOnce = 10240;	/* how many lines to process in a row? */
static ruleset_t *pBindRuleset = NULL;	/* ruleset to bind listener to (use system default if unspecified) */

static int iFilPtr = 0;		/* number of files to be monitored; pointer to next free spot during config */
#define MAX_INPUT_FILES 100
static fileInfo_t files[MAX_INPUT_FILES];

static prop_t *pInputName = NULL;	/* there is only one global inputName for all messages generated by this input */

/* enqueue the read file line as a message. The provided string is
 * not freed - thuis must be done by the caller.
 */
static rsRetVal enqLine(fileInfo_t *pInfo, cstr_t *cstrLine)
{
	DEFiRet;
	msg_t *pMsg;

	if(rsCStrLen(cstrLine) == 0) {
		/* we do not process empty lines */
		FINALIZE;
	}

	CHKiRet(msgConstruct(&pMsg));
	MsgSetFlowControlType(pMsg, eFLOWCTL_FULL_DELAY);
	MsgSetInputName(pMsg, pInputName);
	MsgSetRawMsg(pMsg, (char*)rsCStrGetSzStr(cstrLine), cstrLen(cstrLine));
	MsgSetMSGoffs(pMsg, 0);	/* we do not have a header... */
	MsgSetHOSTNAME(pMsg, glbl.GetLocalHostName(), ustrlen(glbl.GetLocalHostName()));
	MsgSetTAG(pMsg, pInfo->pszTag, pInfo->lenTag);
	pMsg->iFacility = LOG_FAC(pInfo->iFacility);
	pMsg->iSeverity = LOG_PRI(pInfo->iSeverity);
	MsgSetRuleset(pMsg, pInfo->pRuleset);
	pInfo->multiSub.ppMsgs[pInfo->multiSub.nElem++] = pMsg;
	if(pInfo->multiSub.nElem == pInfo->multiSub.maxElem)
		CHKiRet(multiSubmitMsg(&pInfo->multiSub));
finalize_it:
	RETiRet;
}


/* try to open a file. This involves checking if there is a status file and,
 * if so, reading it in. Processing continues from the last know location.
 */
static rsRetVal
openFile(fileInfo_t *pThis)
{
	DEFiRet;
	strm_t *psSF = NULL;
	uchar pszSFNam[MAXFNAME];
	size_t lenSFNam;
	struct stat stat_buf;

	/* Construct file name */
	lenSFNam = snprintf((char*)pszSFNam, sizeof(pszSFNam) / sizeof(uchar), "%s/%s",
			     (char*) glbl.GetWorkDir(), (char*)pThis->pszStateFile);

	/* check if the file exists */
	if(stat((char*) pszSFNam, &stat_buf) == -1) {
		if(errno == ENOENT) {
			dbgprintf("filemon %p: clean startup, no .si file found\n", pThis);
			ABORT_FINALIZE(RS_RET_FILE_NOT_FOUND);
		} else {
			dbgprintf("filemon %p: error %d trying to access .si file\n", pThis, errno);
			ABORT_FINALIZE(RS_RET_IO_ERROR);
		}
	}

	/* If we reach this point, we have a .si file */

	CHKiRet(strm.Construct(&psSF));
	CHKiRet(strm.SettOperationsMode(psSF, STREAMMODE_READ));
	CHKiRet(strm.SetsType(psSF, STREAMTYPE_FILE_SINGLE));
	CHKiRet(strm.SetFName(psSF, pszSFNam, lenSFNam));
	CHKiRet(strm.ConstructFinalize(psSF));

	/* read back in the object */
	CHKiRet(obj.Deserialize(&pThis->pStrm, (uchar*) "strm", psSF, NULL, pThis));

	CHKiRet(strm.SeekCurrOffs(pThis->pStrm));

	/* note: we do not delete the state file, so that the last position remains
	 * known even in the case that rsyslogd aborts for some reason (like powerfail)
	 */

finalize_it:
	if(psSF != NULL)
		strm.Destruct(&psSF);

	if(iRet != RS_RET_OK) {
		CHKiRet(strm.Construct(&pThis->pStrm));
		CHKiRet(strm.SettOperationsMode(pThis->pStrm, STREAMMODE_READ));
		CHKiRet(strm.SetsType(pThis->pStrm, STREAMTYPE_FILE_MONITOR));
		CHKiRet(strm.SetFName(pThis->pStrm, pThis->pszFileName, strlen((char*) pThis->pszFileName)));
		CHKiRet(strm.ConstructFinalize(pThis->pStrm));
	}

	RETiRet;
}


/* The following is a cancel cleanup handler for strmReadLine(). It is necessary in case
 * strmReadLine() is cancelled while processing the stream. -- rgerhards, 2008-03-27
 */
static void pollFileCancelCleanup(void *pArg)
{
	BEGINfunc;
	cstr_t **ppCStr = (cstr_t**) pArg;
	if(*ppCStr != NULL)
		rsCStrDestruct(ppCStr);
	ENDfunc;
}


/* poll a file, need to check file rollover etc. open file if not open */
#pragma GCC diagnostic ignored "-Wempty-body"
static rsRetVal pollFile(fileInfo_t *pThis, int *pbHadFileData)
{
	cstr_t *pCStr = NULL;
	int nProcessed = 0;
	DEFiRet;

	ASSERT(pbHadFileData != NULL);

	/* Note: we must do pthread_cleanup_push() immediately, because the POXIS macros
	 * otherwise do not work if I include the _cleanup_pop() inside an if... -- rgerhards, 2008-08-14
	 */
	pthread_cleanup_push(pollFileCancelCleanup, &pCStr);
	if(pThis->pStrm == NULL) {
		CHKiRet(openFile(pThis)); /* open file */
	}

	/* loop below will be exited when strmReadLine() returns EOF */
	while(glbl.GetGlobalInputTermState() == 0) {
		if(pThis->maxLinesAtOnce != 0 && nProcessed >= pThis->maxLinesAtOnce)
			break;
		CHKiRet(strm.ReadLine(pThis->pStrm, &pCStr, pThis->readMode));
		++nProcessed;
		*pbHadFileData = 1; /* this is just a flag, so set it and forget it */
		CHKiRet(enqLine(pThis, pCStr)); /* process line */
		rsCStrDestruct(&pCStr); /* discard string (must be done by us!) */
		if(pThis->iPersistStateInterval > 0 && pThis->nRecords++ >= pThis->iPersistStateInterval) {
			persistStrmState(pThis);
			pThis->nRecords = 0;
		}
	}

finalize_it:
	if(pThis->multiSub.nElem > 0) {
		/* submit everything that was not yet submitted */
		CHKiRet(multiSubmitMsg(&pThis->multiSub));
	}
		; /*EMPTY STATEMENT - needed to keep compiler happy - see below! */
	/* Note: the problem above is that pthread:cleanup_pop() is a macro which
	 * evaluates to something like "} while(0);". So the code would become
	 * "finalize_it: }", that is a label without a statement. The C standard does
	 * not permit this. So we add an empty statement "finalize_it: ; }" and
	 * everybody is happy. Note that without the ;, an error is reported only
	 * on some platforms/compiler versions. -- rgerhards, 2008-08-15
	 */
	pthread_cleanup_pop(0);

	if(pCStr != NULL) {
		rsCStrDestruct(&pCStr);
	}

	RETiRet;
}
#pragma GCC diagnostic warning "-Wempty-body"


/* This function is the cancel cleanup handler. It is called when rsyslog decides the
 * module must be stopped, what most probably happens during shutdown of rsyslogd. When
 * this function is called, the runInput() function (below) is already terminated - somewhere
 * in the middle of what it was doing. The cancel cleanup handler below should take
 * care of any locked mutexes and such, things that really need to be cleaned up
 * before processing continues. In general, many plugins do not need to provide
 * any code at all here.
 *
 * IMPORTANT: the calling interface of this function can NOT be modified. It actually is
 * called by pthreads. The provided argument is currently not being used.
 */
static void
inputModuleCleanup(void __attribute__((unused)) *arg)
{
	BEGINfunc
	ENDfunc
}


/* This function is called by the framework to gather the input. The module stays
 * most of its lifetime inside this function. It MUST NEVER exit this function. Doing
 * so would end module processing and rsyslog would NOT reschedule the module. If
 * you exit from this function, you violate the interface specification!
 *
 * We go through all files and remember if at least one had data. If so, we do
 * another run (until no data was present in any file). Then we sleep for
 * PollInterval seconds and restart the whole process. This ensures that as
 * long as there is some data present, it will be processed at the fastest
 * possible pace - probably important for busy systmes. If we monitor just a
 * single file, the algorithm is slightly modified. In that case, the sleep
 * hapens immediately. The idea here is that if we have just one file, we
 * returned from the file processer because that file had no additional data.
 * So even if we found some lines, it is highly unlikely to find a new one
 * just now. Trying it would result in a performance-costly additional try
 * which in the very, very vast majority of cases will never find any new
 * lines. 
 * On spamming the main queue: keep in mind that it will automatically rate-limit
 * ourselfes if we begin to overrun it. So we really do not need to care here.
 */
#pragma GCC diagnostic ignored "-Wempty-body"
BEGINrunInput
	int i;
	int bHadFileData; /* were there at least one file with data during this run? */
CODESTARTrunInput
	pthread_cleanup_push(inputModuleCleanup, NULL);
	while(glbl.GetGlobalInputTermState() == 0) {
		do {
			bHadFileData = 0;
			for(i = 0 ; i < iFilPtr ; ++i) {
				if(glbl.GetGlobalInputTermState() == 1)
					break; /* terminate input! */
				pollFile(&files[i], &bHadFileData);
			}
		} while(iFilPtr > 1 && bHadFileData == 1 && glbl.GetGlobalInputTermState() == 0); /* warning: do...while()! */

		/* Note: the additional 10ns wait is vitally important. It guards rsyslog against totally
		 * hogging the CPU if the users selects a polling interval of 0 seconds. It doesn't hurt any
		 * other valid scenario. So do not remove. -- rgerhards, 2008-02-14
		 */
		if(glbl.GetGlobalInputTermState() == 0)
			srSleep(iPollInterval, 10);
	}
	DBGPRINTF("imfile: terminating upon request of rsyslog core\n");
	
	pthread_cleanup_pop(0); /* just for completeness, but never called... */
	RETiRet;	/* use it to make sure the housekeeping is done! */
ENDrunInput
#pragma GCC diagnostic warning "-Wempty-body"
	/* END no-touch zone                                                                          *
	 * ------------------------------------------------------------------------------------------ */



/* The function is called by rsyslog before runInput() is called. It is a last chance
 * to set up anything specific. Most importantly, it can be used to tell rsyslog if the
 * input shall run or not. The idea is that if some config settings (or similiar things)
 * are not OK, the input can tell rsyslog it will not execute. To do so, return
 * RS_RET_NO_RUN or a specific error code. If RS_RET_OK is returned, rsyslog will
 * proceed and call the runInput() entry point.
 */
BEGINwillRun
CODESTARTwillRun
	/* free config variables we do no longer needed */
	free(pszFileName);
	free(pszFileTag);
	free(pszStateFile);

	if(iFilPtr == 0) {
		errmsg.LogError(0, RS_RET_NO_RUN, "No files configured to be monitored");
		ABORT_FINALIZE(RS_RET_NO_RUN);
	}

	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.Construct(&pInputName));
	CHKiRet(prop.SetString(pInputName, UCHAR_CONSTANT("imfile"), sizeof("imfile") - 1));
	CHKiRet(prop.ConstructFinalize(pInputName));

finalize_it:
ENDwillRun



/* This function persists information for a specific file being monitored.
 * To do so, it simply persists the stream object. We do NOT abort on error
 * iRet as that makes matters worse (at least we can try persisting the others...).
 * rgerhards, 2008-02-13
 */
static rsRetVal
persistStrmState(fileInfo_t *pInfo)
{
	DEFiRet;
	strm_t *psSF = NULL; /* state file (stream) */
	size_t lenDir;

	ASSERT(pInfo != NULL);

	/* TODO: create a function persistObj in obj.c? */
	CHKiRet(strm.Construct(&psSF));
	lenDir = ustrlen(glbl.GetWorkDir());
	if(lenDir > 0)
		CHKiRet(strm.SetDir(psSF, glbl.GetWorkDir(), lenDir));
	CHKiRet(strm.SettOperationsMode(psSF, STREAMMODE_WRITE_TRUNC));
	CHKiRet(strm.SetsType(psSF, STREAMTYPE_FILE_SINGLE));
	CHKiRet(strm.SetFName(psSF, pInfo->pszStateFile, strlen((char*) pInfo->pszStateFile)));
	CHKiRet(strm.ConstructFinalize(psSF));

	CHKiRet(strm.Serialize(pInfo->pStrm, psSF));

	CHKiRet(strm.Destruct(&psSF));

finalize_it:
	if(psSF != NULL)
		strm.Destruct(&psSF);

	RETiRet;
}


/* This function is called by the framework after runInput() has been terminated. It
 * shall free any resources and prepare the module for unload.
 */
BEGINafterRun
	int i;
CODESTARTafterRun
	/* Close files and persist file state information. We do NOT abort on error iRet as that makes
	 * matters worse (at least we can try persisting the others...). Please note that, under stress
	 * conditions, it may happen that we are terminated before we actuall could open all streams. So
	 * before we change anything, we need to make sure the stream was open.
	 */
	for(i = 0 ; i < iFilPtr ; ++i) {
		if(files[i].pStrm != NULL) { /* stream open? */
			persistStrmState(&files[i]);
			strm.Destruct(&(files[i].pStrm));
		}
		free(files[i].pszFileName);
		free(files[i].pszTag);
		free(files[i].pszStateFile);
	}

	if(pInputName != NULL)
		prop.Destruct(&pInputName);
ENDafterRun


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURENonCancelInputTermination)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


/* The following entry points are defined in module-template.h.
 * In general, they need to be present, but you do NOT need to provide
 * any code here.
 */
BEGINmodExit
CODESTARTmodExit
	/* release objects we used */
	objRelease(strm, CORE_COMPONENT);
	objRelease(datetime, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
	objRelease(ruleset, CORE_COMPONENT);
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
ENDqueryEtryPt


/* The following function shall reset all configuration variables to their
 * default values. The code provided in modInit() below registers it to be
 * called on "$ResetConfigVariables". You may also call it from other places,
 * but in general this is not necessary. Once runInput() has been called, this
 * function here is never again called.
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	DEFiRet;

	if(pszFileName != NULL) {
		free(pszFileName);
		pszFileName = NULL;
	}

	if(pszFileTag != NULL) {
		free(pszFileTag);
		pszFileTag = NULL;
	}

	if(pszStateFile != NULL) {
		free(pszFileTag);
		pszFileTag = NULL;
	}


	/* set defaults... */
	iPollInterval = 10;
	iFacility = 128; /* local0 */
	iSeverity = 5;  /* notice, as of rfc 3164 */
	readMode = 0;
	pBindRuleset = NULL;
	maxLinesAtOnce = 10240;

	RETiRet;
}


/* add a new monitor */
static rsRetVal addMonitor(void __attribute__((unused)) *pVal, uchar *pNewVal)
{
	DEFiRet;
	fileInfo_t *pThis;

	free(pNewVal); /* we do not need it, but we must free it! */

	if(iFilPtr < MAX_INPUT_FILES) {
		pThis = &files[iFilPtr];
		/* TODO: check for strdup() NULL return */
		if(pszFileName == NULL) {
			errmsg.LogError(0, RS_RET_CONFIG_ERROR, "imfile error: no file name given, file monitor can not be created");
			ABORT_FINALIZE(RS_RET_CONFIG_ERROR);
		} else {
			pThis->pszFileName = (uchar*) strdup((char*) pszFileName);
		}

		if(pszFileTag == NULL) {
			errmsg.LogError(0, RS_RET_CONFIG_ERROR, "imfile error: no tag value given , file monitor can not be created");
			ABORT_FINALIZE(RS_RET_CONFIG_ERROR);
		} else {
			pThis->pszTag = (uchar*) strdup((char*) pszFileTag);
			pThis->lenTag = ustrlen(pThis->pszTag);
		}

		if(pszStateFile == NULL) {
			errmsg.LogError(0, RS_RET_CONFIG_ERROR, "imfile error: not state file name given, file monitor can not be created");
			ABORT_FINALIZE(RS_RET_CONFIG_ERROR);
		} else {
			pThis->pszStateFile = (uchar*) strdup((char*) pszStateFile);
		}

		CHKmalloc(pThis->multiSub.ppMsgs = MALLOC(NUM_MULTISUB * sizeof(msg_t*)));
		pThis->multiSub.maxElem = NUM_MULTISUB;
		pThis->multiSub.nElem = 0;
		pThis->iSeverity = iSeverity;
		pThis->iFacility = iFacility;
		pThis->maxLinesAtOnce = maxLinesAtOnce;
		pThis->iPersistStateInterval = iPersistStateInterval;
		pThis->nRecords = 0;
		pThis->readMode = readMode;
		pThis->pRuleset = pBindRuleset;
		iPersistStateInterval = 0;
	} else {
		errmsg.LogError(0, RS_RET_OUT_OF_DESRIPTORS, "Too many file monitors configured - ignoring this one");
		ABORT_FINALIZE(RS_RET_OUT_OF_DESRIPTORS);
	}

	CHKiRet(resetConfigVariables((uchar*) "dummy", (void*) pThis)); /* values are both dummies */

finalize_it:
	if(iRet == RS_RET_OK)
		++iFilPtr;	/* we got a new file to monitor */

	RETiRet;
}


/* accept a new ruleset to bind. Checks if it exists and complains, if not */
static rsRetVal
setRuleset(void __attribute__((unused)) *pVal, uchar *pszName)
{
	ruleset_t *pRuleset;
	rsRetVal localRet;
	DEFiRet;

	localRet = ruleset.GetRuleset(ourConf, &pRuleset, pszName);
	if(localRet == RS_RET_NOT_FOUND) {
		errmsg.LogError(0, NO_ERRCODE, "error: ruleset '%s' not found - ignored", pszName);
	}
	CHKiRet(localRet);
	pBindRuleset = pRuleset;
	DBGPRINTF("imfile current bind ruleset %p: '%s'\n", pRuleset, pszName);

finalize_it:
	free(pszName); /* no longer needed */
	RETiRet;
}


/* modInit() is called once the module is loaded. It must perform all module-wide
 * initialization tasks. There are also a number of housekeeping tasks that the
 * framework requires. These are handled by the macros. Please note that the
 * complexity of processing is depending on the actual module. However, only
 * thing absolutely necessary should be done here. Actual app-level processing
 * is to be performed in runInput(). A good sample of what to do here may be to
 * set some variable defaults. The most important thing probably is registration
 * of config command handlers.
 */
BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	CHKiRet(objUse(strm, CORE_COMPONENT));
	CHKiRet(objUse(ruleset, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));

	DBGPRINTF("imfile: version %s initializing\n", VERSION);
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilename", 0, eCmdHdlrGetWord,
	  	NULL, &pszFileName, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfiletag", 0, eCmdHdlrGetWord,
	  	NULL, &pszFileTag, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilestatefile", 0, eCmdHdlrGetWord,
	  	NULL, &pszStateFile, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfileseverity", 0, eCmdHdlrSeverity,
	  	NULL, &iSeverity, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilefacility", 0, eCmdHdlrFacility,
	  	NULL, &iFacility, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilepollinterval", 0, eCmdHdlrInt,
	  	NULL, &iPollInterval, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilereadmode", 0, eCmdHdlrInt,
	  	NULL, &readMode, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilemaxlinesatonce", 0, eCmdHdlrSize,
	  	NULL, &maxLinesAtOnce, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilepersiststateinterval", 0, eCmdHdlrInt,
	  	NULL, &iPersistStateInterval, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputfilebindruleset", 0, eCmdHdlrGetWord,
		setRuleset, NULL, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	/* that command ads a new file! */
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputrunfilemonitor", 0, eCmdHdlrGetWord,
		addMonitor, NULL, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
		resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID, eConfObjGlobal));
ENDmodInit
/* vim:set ai:
 */
