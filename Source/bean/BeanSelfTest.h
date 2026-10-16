// Command-line self-test driver for the Bean data layer.
//
// Why this exists in addition to BeanAutomationTests.cpp: the editor's automation
// framework never leaves its FindWorkers state on this machine (no worker session is
// available headlessly), so "Automation RunTests Bean" queues nothing and the editor
// then idles forever without reporting a result. This driver runs the same production
// code paths through a console command instead, so the features can be verified from
// the command line with real timings and a real pass/fail list:
//
//   UnrealEditor-Cmd.exe D:\bean\bean.uproject -ExecCmds="BeanSelfTest; Quit" -unattended -nopause -nosplash
//
// It only ever reads/modifies its own temporary work slots (__BeanSelfTest*) and its own
// temporary canvas; it never touches the player's own saved works. A plain-text copy of
// the report is written to Saved/BeanSelfTestReport.txt so the evidence survives even if
// the editor process is killed before it flushes its log.

#pragma once

#include "CoreMinimal.h"

class UWorld;

class BEAN_API FBeanSelfTest
{
public:
	/**
	 * Runs every data-layer check that does not need a game world (image conversion,
	 * transparency, error handling, automatic crop).
	 *
	 * @param LogDetail  log one line per individual assertion instead of only failures
	 * @return           number of failed checks
	 */
	static int32 RunImageChecks(bool LogDetail);

	/**
	 * Runs every check that needs a live world (undo/redo, batch edits, completion
	 * statistics, save/load reliability, large-canvas measurements).
	 *
	 * @param World      world to spawn the temporary grid actor in
	 * @param LogDetail  log one line per individual assertion instead of only failures
	 * @return           number of failed checks
	 */
	static int32 RunGridChecks(UWorld* World, bool LogDetail);

	/**
	 * Runs both halves, writes Saved/BeanSelfTestReport.txt and returns the total number
	 * of failed checks.
	 */
	static int32 RunAll(UWorld* World, bool LogDetail);
};
