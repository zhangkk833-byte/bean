// Self-tests for the Bean data layer.
//
// These deliberately avoid Slate, UMG, file dialogs and rendering of the board: they exercise the
// pure data paths (image conversion, undo/redo, batch edits, completion statistics, save/load and
// the large-canvas cost) so the eight features can be verified from the command line with real
// numbers instead of by clicking through the game.
//
// Run with:
//   UnrealEditor-Cmd.exe D:\bean\bean.uproject -ExecCmds="Automation RunTests Bean; Quit" -unattended -nopause -nullrhi -nosplash -log
//
// The whole file is editor-only: the large-canvas measurements need GEditor for a world to spawn
// the grid actor in, and nothing here is meant to ship in a cooked game.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "BeanGrid.h"
#include "BeanImageConverter.h"
#include "BeanPalette.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "Misc/Paths.h"

namespace BeanTestPrivate
{
	/** Deterministic picture generator: a gradient plus a few hard-edged blocks.
	 *  Hard edges matter because the quantizer's edge handling is exactly what is under test. */
	static void BuildTestImage(FImage& OutImage, int32 Width, int32 Height, bool bWithTransparentBorder)
	{
		OutImage.Init(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

		FColor* Pixels = OutImage.AsBGRA8().GetData();
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const float U = static_cast<float>(X) / static_cast<float>(FMath::Max(1, Width - 1));
				const float V = static_cast<float>(Y) / static_cast<float>(FMath::Max(1, Height - 1));
				FColor& Pixel = Pixels[Y * Width + X];

				if (bWithTransparentBorder && (X < 4 || Y < 4 || X >= Width - 4 || Y >= Height - 4))
				{
					Pixel = FColor(0, 0, 0, 0);
					continue;
				}

				// A red/blue gradient with a green wedge: three well-separated hues, so a
				// reduction to a small colour count has something meaningful to merge.
				const bool bWedge = (X > Width / 3) && (X < (2 * Width) / 3) && (Y < Height / 2);
				if (bWedge)
				{
					Pixel = FColor(20, static_cast<uint8>(FMath::Clamp(80 + 160 * V, 0.f, 255.f)), 40, 255);
				}
				else
				{
					Pixel = FColor(static_cast<uint8>(FMath::Clamp(40 + 200 * U, 0.f, 255.f)), 60,
						static_cast<uint8>(FMath::Clamp(220 - 180 * V, 0.f, 255.f)), 255);
				}
			}
		}
	}

	/** Distinct palette indices in a pattern, ignoring empty cells. */
	static int32 CountUsedColors(const TArray<uint8>& Cells, uint8 EmptyValue)
	{
		TSet<uint8> Used;
		for (const uint8 Cell : Cells)
		{
			if (Cell != EmptyValue)
			{
				Used.Add(Cell);
			}
		}
		return Used.Num();
	}

	/** Highest palette index found in a pattern. */
	static int32 MaxIndex(const TArray<uint8>& Cells, uint8 EmptyValue)
	{
		int32 Max = -1;
		for (const uint8 Cell : Cells)
		{
			if (Cell != EmptyValue)
			{
				Max = FMath::Max(Max, static_cast<int32>(Cell));
			}
		}
		return Max;
	}

	/** Find a palette index different from Both and from AnyOf (used to build "wrong colour" cases). */
	static int32 FindOtherColor(const TArray<FBeanPaletteColor>& Palette, int32 A, int32 B)
	{
		for (int32 Index = 0; Index < Palette.Num(); ++Index)
		{
			if (Index != A && Index != B)
			{
				return Index;
			}
		}
		return -1;
	}
}

// ---------------------------------------------------------------------------
// Feature 1: image conversion quality
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeanConversionTest,
	"Bean.Convert.ImageToPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeanConversionTest::RunTest(const FString& Parameters)
{
	using namespace BeanTestPrivate;

	const TArray<FBeanPaletteColor> Palette = FBeanPalette::LoadOrCreateDefault();
	TestEqual(TEXT("palette size is the shipped 48"), Palette.Num(), 48);

	FImage Source;
	BuildTestImage(Source, 512, 512, /*bWithTransparentBorder=*/false);

	FBeanConvertOptions Options;
	Options.CanvasWidth = 64;
	Options.CanvasHeight = 64;
	FBeanImageConverter::SetFullCrop(Options, Source);

	FBeanConvertResult First;
	TestTrue(TEXT("conversion of a plain RGB picture succeeds"), FBeanImageConverter::Convert(Source, Palette, Options, First));
	TestEqual(TEXT("pattern is canvas sized"), First.Cells.Num(), 64 * 64);
	TestTrue(TEXT("no error message on success"), First.Error.IsEmpty());
	TestEqual(TEXT("PlacedCells matches the non-empty cells"), First.PlacedCells, First.Cells.Num());
	TestTrue(TEXT("at least one color is used"), First.ColorCount > 0);

	// Acceptance criterion: "same picture + same settings = same output".
	FBeanConvertResult Second;
	TestTrue(TEXT("second conversion succeeds"), FBeanImageConverter::Convert(Source, Palette, Options, Second));
	TestEqual(TEXT("re-running the same conversion is bit-identical (determinism)"), Second.Cells, First.Cells);
	TestEqual(TEXT("the reported color count is stable"), Second.ColorCount, First.ColorCount);

	// Acceptance criterion: "output colors belong to the real palette".
	TestTrue(TEXT("every emitted index is inside the palette"), MaxIndex(First.Cells, Options.EmptyValue) < Palette.Num());

	// Color-count control: the subset must come from the picture and stay within the limit.
	FBeanConvertOptions Limited = Options;
	Limited.MaxColors = 8;
	FBeanConvertResult Reduced;
	TestTrue(TEXT("conversion with a color limit succeeds"), FBeanImageConverter::Convert(Source, Palette, Limited, Reduced));
	const int32 ReducedColors = CountUsedColors(Reduced.Cells, Limited.EmptyValue);
	TestTrue(FString::Printf(TEXT("8-color limit is respected (actually used %d)"), ReducedColors), ReducedColors <= 8);
	TestTrue(TEXT("the limited palette is still reported consistently"), Reduced.ColorCount == ReducedColors);

	// Transparency: a transparent border must become holes, not black beads.
	FImage TransparentSource;
	BuildTestImage(TransparentSource, 256, 256, /*bWithTransparentBorder=*/true);
	FBeanConvertOptions TransparentOptions;
	TransparentOptions.CanvasWidth = 32;
	TransparentOptions.CanvasHeight = 32;
	FBeanImageConverter::SetFullCrop(TransparentOptions, TransparentSource);

	FBeanConvertResult TransparentResult;
	TestTrue(TEXT("conversion of a picture with alpha succeeds"), FBeanImageConverter::Convert(TransparentSource, Palette, TransparentOptions, TransparentResult));
	TestTrue(TEXT("transparency is detected"), TransparentResult.bHasTransparency);
	TestTrue(TEXT("the transparent corner stays empty"), TransparentResult.Cells[0] == TransparentOptions.EmptyValue);
	TestTrue(TEXT("the opaque middle gets a bead"), TransparentResult.Cells[(32 / 2) * 32 + (32 / 2)] != TransparentOptions.EmptyValue);
	TestTrue(TEXT("PlacedCells excludes the transparent holes"), TransparentResult.PlacedCells < TransparentResult.Cells.Num());

	// Acceptance criterion: bad input is reported, not crashed on.
	FImage Empty;
	FBeanConvertResult BadResult;
	TestFalse(TEXT("an empty image is rejected instead of crashing"), FBeanImageConverter::Convert(Empty, Palette, Options, BadResult));
	TestTrue(TEXT("the rejection carries a readable reason"), !BadResult.Error.IsEmpty());

	FImage MissingFile;
	FString MissingError;
	TestFalse(TEXT("a missing file is reported, not fatal"),
		FBeanImageConverter::LoadSourceImage(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("does_not_exist_bean.png")), MissingFile, MissingError));
	TestTrue(TEXT("the missing-file error is readable"), !MissingError.IsEmpty());

	return true;
}

// ---------------------------------------------------------------------------
// Feature 5: automatic crop
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeanAutoCropTest,
	"Bean.Convert.AutoCropKeepsSubject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeanAutoCropTest::RunTest(const FString& Parameters)
{
	using namespace BeanTestPrivate;

	// A dark subject on a light background: the crop suggestion should hug the subject (plus a
	// margin) rather than returning the whole picture.
	FImage Source;
	Source.Init(200, 200, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Pixels = Source.AsBGRA8().GetData();
	for (int32 Y = 0; Y < 200; ++Y)
	{
		for (int32 X = 0; X < 200; ++X)
		{
			const bool bSubject = X >= 60 && X < 140 && Y >= 50 && Y < 150;
			Pixels[Y * 200 + X] = bSubject ? FColor(30, 30, 30, 255) : FColor(240, 240, 240, 255);
		}
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	const bool bSuggested = FBeanImageConverter::SuggestCrop(Source, 32, 32, MinX, MinY, MaxX, MaxY);
	TestTrue(TEXT("a clear subject yields a crop suggestion"), bSuggested);
	if (bSuggested)
	{
		TestTrue(TEXT("crop excludes the empty left margin"), MinX > 0);
		TestTrue(TEXT("crop excludes the empty top margin"), MinY > 0);
		TestTrue(TEXT("crop excludes the empty right margin"), MaxX < 199);
		TestTrue(TEXT("crop excludes the empty bottom margin"), MaxY < 199);
		TestTrue(TEXT("crop covers the subject horizontally"), MinX <= 60 && MaxX >= 139);
		TestTrue(TEXT("crop covers the subject vertically"), MinY <= 50 && MaxY >= 149);
	}

	// A flat picture has no subject to find: the call must say so instead of guessing.
	FImage Flat;
	Flat.Init(64, 64, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memset(Flat.AsBGRA8().GetData(), 0x80, static_cast<SIZE_T>(Flat.AsBGRA8().Num()) * sizeof(FColor));
	const bool bFlatSuggested = FBeanImageConverter::SuggestCrop(Flat, 32, 32, MinX, MinY, MaxX, MaxY);
	TestFalse(TEXT("a subjectless picture reports that it cannot decide"), bFlatSuggested);

	return true;
}

// ---------------------------------------------------------------------------
// Features 2/3/4 + 8: grid data layer, history, batch edits, statistics, cost
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeanGridDataTest,
	"Bean.Grid.EditsHistoryAndStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeanGridDataTest::RunTest(const FString& Parameters)
{
	using namespace BeanTestPrivate;

	if (!GEditor)
	{
		AddError(TEXT("GEditor is unavailable; this test needs the editor context."));
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddError(TEXT("no editor world available to spawn the grid actor in."));
		return false;
	}

	ABeanGrid* Grid = World->SpawnActor<ABeanGrid>();
	if (!Grid)
	{
		AddError(TEXT("could not spawn ABeanGrid."));
		return false;
	}
	Grid->DispatchBeginPlay();

	TestTrue(TEXT("grid initialized with a valid cell array"), Grid->IsReady());
	TestEqual(TEXT("default canvas is 256x256"), Grid->GetWidth(), 256);
	TestEqual(TEXT("default canvas is 256x256 (height)"), Grid->GetHeight(), 256);

	// --- undo / redo of single edits ---------------------------------------------------------
	const int32 ColorA = 1;
	const int32 ColorB = 4;
	Grid->SetCurrentColor(ColorA);
	Grid->ClearHistory();

	Grid->PlaceCell(10, 10);
	TestEqual(TEXT("placement landed in the data"), Grid->GetCell(10, 10), static_cast<uint8>(ColorA));
	TestTrue(TEXT("one undo step is available"), Grid->CanUndo());
	TestFalse(TEXT("nothing to redo yet"), Grid->CanRedo());

	Grid->Undo();
	TestEqual(TEXT("undo removed the bead"), Grid->GetCell(10, 10), static_cast<uint8>(ABeanGrid::EmptyCell));
	TestFalse(TEXT("undo consumed the history"), Grid->CanUndo());
	TestTrue(TEXT("redo is now available"), Grid->CanRedo());

	Grid->Redo();
	TestEqual(TEXT("redo put the bead back with the same color"), Grid->GetCell(10, 10), static_cast<uint8>(ColorA));

	// A new edit must invalidate the redo stack (no "redo into a lie").
	Grid->Undo();
	Grid->SetCurrentColor(ColorB);
	Grid->PlaceCell(11, 11);
	TestFalse(TEXT("a new edit clears the redo stack"), Grid->CanRedo());

	// --- one drag = one undo step ------------------------------------------------------------
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetCurrentColor(ColorA);
	Grid->BeginStroke(TEXT("continuous placement"));
	for (int32 X = 20; X < 30; ++X)
	{
		Grid->PlaceCell(X, 20);
	}
	Grid->EndStroke();
	TestEqual(TEXT("a whole drag is a single undo step"), Grid->GetUndoCount(), 1);
	Grid->Undo();
	bool bStrokeFullyUndone = true;
	for (int32 X = 20; X < 30; ++X)
	{
		bStrokeFullyUndone &= (Grid->GetCell(X, 20) == ABeanGrid::EmptyCell);
	}
	TestTrue(TEXT("undoing the stroke removes every bead of the drag"), bStrokeFullyUndone);

	// --- batch operations --------------------------------------------------------------------
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetCurrentColor(ColorA);
	const int32 Placed = Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 30, 30, 39, 39);
	TestEqual(TEXT("batch place fills the whole rectangle"), Placed, 100);
	TestEqual(TEXT("batch place writes the corner"), Grid->GetCell(30, 30), static_cast<uint8>(ColorA));
	TestEqual(TEXT("batch place writes the far corner"), Grid->GetCell(39, 39), static_cast<uint8>(ColorA));
	TestEqual(TEXT("one batch is one undo step"), Grid->GetUndoCount(), 1);

	const int32 Replaced = Grid->ApplyBatch(EBeanBatchOp::ReplaceColor, 30, 30, 39, 39, static_cast<uint8>(ColorA), static_cast<uint8>(ColorB));
	TestEqual(TEXT("batch replace converts every bead in the rectangle"), Replaced, 100);
	TestEqual(TEXT("batch replace changed the color"), Grid->GetCell(35, 35), static_cast<uint8>(ColorB));

	const int32 Removed = Grid->ApplyBatch(EBeanBatchOp::Remove, 30, 30, 39, 39);
	TestEqual(TEXT("batch remove clears the rectangle"), Removed, 100);
	TestEqual(TEXT("batch remove left an empty cell"), Grid->GetCell(35, 35), static_cast<uint8>(ABeanGrid::EmptyCell));
	TestEqual(TEXT("the three batch operations are three undo steps"), Grid->GetUndoCount(), 3);

	// A batch that changes nothing must not create a history entry.
	const int32 Noop = Grid->ApplyBatch(EBeanBatchOp::Remove, 30, 30, 39, 39);
	TestEqual(TEXT("a no-op batch reports zero changes"), Noop, 0);
	TestEqual(TEXT("a no-op batch adds no history"), Grid->GetUndoCount(), 3);

	// --- target pattern and completion statistics --------------------------------------------
	// A 4x4 target: 16 cells, one of them empty (the pattern has a hole), so the denominator is 15.
	const int32 PatternW = 4;
	const int32 PatternH = 4;
	const int32 OriginX = 100;
	const int32 OriginY = 100;
	TArray<uint8> Target;
	TArray<bool> Mask;
	Target.SetNumZeroed(PatternW * PatternH);
	Mask.Init(true, PatternW * PatternH);
	for (int32 I = 0; I < PatternW * PatternH; ++I)
	{
		Target[I] = static_cast<uint8>(ColorA);
	}
	Target[0] = ABeanGrid::EmptyCell;   // the pattern explicitly asks for nothing here
	Grid->SetTargetPattern(PatternW, PatternH, OriginX, OriginY, Target, Mask);
	TestTrue(TEXT("the target pattern is registered"), Grid->HasTargetPattern());

	Grid->ClearAllCells();
	Grid->ClearHistory();

	FBeanCompletionStats Stats;
	Grid->GetCompletionStats(Stats);
	TestTrue(TEXT("statistics report a pattern"), Stats.bHasPattern);
	TestEqual(TEXT("the empty pattern cell is excluded from the target total"), Stats.TargetCells, 15);
	TestEqual(TEXT("nothing is correct before any bead is placed"), Stats.CorrectCells, 0);
	TestEqual(TEXT("every required cell is missing"), Stats.MissingCells, 15);
	TestEqual(TEXT("completion starts at zero"), FMath::RoundToInt(Stats.CompletionPercent), 0);

	// One correct placement, then check the counters and the percentage.
	Grid->SetCurrentColor(ColorA);
	Grid->PlaceCell(OriginX + 1, OriginY);          // correct color
	Grid->GetCompletionStats(Stats);
	TestEqual(TEXT("a correct bead counts as correct"), Stats.CorrectCells, 1);
	TestEqual(TEXT("the missing count drops"), Stats.MissingCells, 14);
	TestEqual(TEXT("completion percent follows the correct cells"), FMath::RoundToInt(Stats.CompletionPercent), FMath::RoundToInt(100.0f * 1.0f / 15.0f));

	// One wrong-color placement must NOT count as correct (the naive "placed / total" metric would).
	Grid->SetCurrentColor(ColorB);
	Grid->PlaceCell(OriginX + 2, OriginY);
	Grid->GetCompletionStats(Stats);
	TestEqual(TEXT("a wrong color is not counted as correct"), Stats.CorrectCells, 1);
	TestEqual(TEXT("the wrong color is reported separately"), Stats.MiscoloredCells, 1);

	// Per-color breakdown has to add up to the totals.
	int32 SumRequired = 0;
	int32 SumCorrect = 0;
	int32 SumRemaining = 0;
	for (const FBeanColorProgress& Entry : Stats.PerColor)
	{
		SumRequired += Entry.Required;
		SumCorrect += Entry.Correct;
		SumRemaining += Entry.Remaining;
	}
	TestEqual(TEXT("per-color required adds up to the target total"), SumRequired, Stats.TargetCells);
	TestEqual(TEXT("per-color correct adds up to the correct total"), SumCorrect, Stats.CorrectCells);
	TestEqual(TEXT("per-color remaining is consistent"), SumRemaining, Stats.TargetCells - Stats.CorrectCells);

	// Undo must roll the statistics back too.
	Grid->Undo();
	Grid->GetCompletionStats(Stats);
	TestEqual(TEXT("undo rolls the statistics back"), Stats.CorrectCells, 1);
	TestEqual(TEXT("undo removes the wrong-color entry"), Stats.MiscoloredCells, 0);

	// --- canvas resize keeps the resolution system intact -----------------------------------
	TestTrue(TEXT("canvas can be resized to 32x32"), Grid->ResizeGrid(32, 32));
	TestEqual(TEXT("width follows the resize"), Grid->GetWidth(), 32);
	TestEqual(TEXT("height follows the resize"), Grid->GetHeight(), 32);
	TestEqual(TEXT("the cell array matches the new size"), Grid->GetCellCount(), 32 * 32);
	TestFalse(TEXT("resizing drops the target pattern"), Grid->HasTargetPattern());

	// The four shipped resolutions must all still be accepted.
	for (const int32 Resolution : ABeanGrid::GetResolutionOptions())
	{
		TestTrue(FString::Printf(TEXT("resolution %d is accepted"), Resolution), Grid->ResizeGrid(Resolution, Resolution));
		TestEqual(FString::Printf(TEXT("resolution %d round-trips"), Resolution), Grid->GetWidth(), Resolution);
	}

	Grid->Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// Feature 8: large-canvas cost, measured
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeanPerfTest,
	"Bean.Perf.LargeCanvasCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeanPerfTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddError(TEXT("GEditor is unavailable; this test needs the editor context."));
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddError(TEXT("no editor world available to spawn the grid actor in."));
		return false;
	}

	ABeanGrid* Grid = World->SpawnActor<ABeanGrid>();
	if (!Grid)
	{
		AddError(TEXT("could not spawn ABeanGrid."));
		return false;
	}
	Grid->DispatchBeginPlay();

	AddInfo(TEXT("--- Bean large-canvas measurements (same machine, same session, editor build) ---"));

	for (const int32 Resolution : ABeanGrid::GetResolutionOptions())
	{
		const double ResizeStart = FPlatformTime::Seconds();
		if (!Grid->ResizeGrid(Resolution, Resolution))
		{
			AddError(FString::Printf(TEXT("resize to %d failed"), Resolution));
			continue;
		}
		const double ResizeMs = (FPlatformTime::Seconds() - ResizeStart) * 1000.0;

		Grid->ClearAllCells();
		Grid->ClearHistory();

		// Batch fill: one entry point for a whole rectangle, which is the operation the batch UI uses.
		const double BatchStart = FPlatformTime::Seconds();
		const int32 Filled = Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, Resolution - 1, Resolution - 1);
		const double BatchMs = (FPlatformTime::Seconds() - BatchStart) * 1000.0;
		if (Filled != Resolution * Resolution)
		{
			AddError(FString::Printf(TEXT("batch fill of %dx%d changed %d cells instead of %d"),
				Resolution, Resolution, Filled, Resolution * Resolution));
		}

		// Single placement / removal, averaged over a sample.
		Grid->ClearAllCells();
		Grid->ClearHistory();
		const int32 Sample = 1000;
		const double PlaceStart = FPlatformTime::Seconds();
		for (int32 I = 0; I < Sample; ++I)
		{
			Grid->PlaceCell(I % Resolution, I / Resolution);
		}
		const double PlaceUs = (FPlatformTime::Seconds() - PlaceStart) * 1.0e6 / Sample;

		const double RemoveStart = FPlatformTime::Seconds();
		for (int32 I = 0; I < Sample; ++I)
		{
			Grid->RemoveCell(I % Resolution, I / Resolution);
		}
		const double RemoveUs = (FPlatformTime::Seconds() - RemoveStart) * 1.0e6 / Sample;

		// Statistics over a full canvas with a pattern present.
		TArray<uint8> Target;
		Target.Init(1, Resolution * Resolution);
		TArray<bool> Mask;
		Mask.Init(true, Resolution * Resolution);
		Grid->SetTargetPattern(Resolution, Resolution, 0, 0, Target, Mask);
		Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, Resolution - 1, Resolution - 1);

		const double StatsStart = FPlatformTime::Seconds();
		FBeanCompletionStats Stats;
		Grid->GetCompletionStats(Stats);
		const double StatsMs = (FPlatformTime::Seconds() - StatsStart) * 1000.0;

		// Save + load round trip of the real file format.
		const FString SlotName = FString::Printf(TEXT("__BeanPerfTest_%d"), Resolution);
		FString SaveError;
		const double SaveStart = FPlatformTime::Seconds();
		const bool bSaved = Grid->SaveGameAs(SlotName, SaveError);
		const double SaveMs = (FPlatformTime::Seconds() - SaveStart) * 1000.0;

		const double LoadStart = FPlatformTime::Seconds();
		FString LoadError;
		const bool bLoaded = Grid->LoadGameFrom(SlotName, LoadError);
		const double LoadMs = (FPlatformTime::Seconds() - LoadStart) * 1000.0;

		if (!bSaved) { AddError(FString::Printf(TEXT("save failed at %d: %s"), Resolution, *SaveError)); }
		if (!bLoaded) { AddError(FString::Printf(TEXT("load failed at %d: %s"), Resolution, *LoadError)); }

		const int64 Bytes = IFileManager::Get().FileSize(*ABeanGrid::GetSlotFilePath(SlotName));

		AddInfo(FString::Printf(
			TEXT("%3dx%-3d | resize %7.2f ms | batch fill %8.2f ms | place %7.2f us | remove %7.2f us | stats %7.2f ms | save %8.2f ms (%lld KB) | load %8.2f ms"),
			Resolution, Resolution, ResizeMs, BatchMs, PlaceUs, RemoveUs, StatsMs, SaveMs, Bytes / 1024, LoadMs));

		// The temp/backup files the robust writer leaves behind must be cleanable.
		IFileManager::Get().Delete(*ABeanGrid::GetSlotFilePath(SlotName), false, true, true);
		IFileManager::Get().Delete(*(ABeanGrid::GetSlotFilePath(SlotName) + TEXT(".bak")), false, true, true);
		IFileManager::Get().Delete(*(ABeanGrid::GetSlotFilePath(SlotName) + TEXT(".tmp")), false, true, true);
	}

	Grid->Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// Save reliability: versioning, corruption, backup recovery
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeanSaveReliabilityTest,
	"Bean.Save.RoundTripBackupAndCorruption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeanSaveReliabilityTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddError(TEXT("GEditor is unavailable; this test needs the editor context."));
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddError(TEXT("no editor world available to spawn the grid actor in."));
		return false;
	}

	ABeanGrid* Grid = World->SpawnActor<ABeanGrid>();
	if (!Grid)
	{
		AddError(TEXT("could not spawn ABeanGrid."));
		return false;
	}
	Grid->DispatchBeginPlay();

	const FString SlotName = TEXT("__BeanSaveReliabilityTest");
	const FString FilePath = ABeanGrid::GetSlotFilePath(SlotName);

	// --- round trip --------------------------------------------------------------------------
	Grid->ResizeGrid(64, 64);
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetWorkName(SlotName);
	Grid->SetCurrentColor(3);
	Grid->PlaceCell(5, 7);
	Grid->SetCurrentColor(9);
	Grid->PlaceCell(40, 41);
	Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 10, 10, 19, 19);

	TArray<uint8> Target;
	Target.Init(9, 64 * 64);
	TArray<bool> Mask;
	Mask.Init(true, 64 * 64);
	Grid->SetTargetPattern(64, 64, 0, 0, Target, Mask);

	FString Error;
	TestTrue(TEXT("save to an explicit slot succeeds"), Grid->SaveGameAs(SlotName, Error));
	TestTrue(TEXT("the save file exists afterwards"), ABeanGrid::DoesSlotExist(SlotName));
	TestTrue(TEXT("a successful save reports success, not failure"), Grid->GetSaveStatusMessage().Contains(TEXT("已保存")));
	TestFalse(TEXT("a successful save clears the dirty flag"), Grid->IsDirty());

	// Dirty a few cells, then reload and check the data came back exactly.
	Grid->ClearAllCells();
	Grid->ClearHistory();
	TestTrue(TEXT("reload succeeds"), Grid->LoadGameFrom(SlotName, Error));
	TestEqual(TEXT("canvas width survived the round trip"), Grid->GetWidth(), 64);
	TestEqual(TEXT("canvas height survived the round trip"), Grid->GetHeight(), 64);
	TestEqual(TEXT("cell (5,7) survived with its color"), Grid->GetCell(5, 7), static_cast<uint8>(3));
	TestEqual(TEXT("cell (40,41) survived with its color"), Grid->GetCell(40, 41), static_cast<uint8>(9));
	TestEqual(TEXT("cell (15,15) survived the batch fill"), Grid->GetCell(15, 15), static_cast<uint8>(9));
	TestTrue(TEXT("the target pattern survived"), Grid->HasTargetPattern());

	FBeanCompletionStats Stats;
	Grid->GetCompletionStats(Stats);
	TestEqual(TEXT("completion survives a save/load round trip"), Stats.CorrectCells, 101);

	// --- backup stays behind and is usable ---------------------------------------------------
	Grid->SetCurrentColor(12);
	Grid->PlaceCell(1, 1);
	TestTrue(TEXT("second save succeeds"), Grid->SaveGameAs(SlotName, Error));
	TestTrue(TEXT("the previous file was kept as a backup"), IFileManager::Get().FileExists(*(FilePath + TEXT(".bak"))));

	// A damaged primary file must fall back to the backup instead of losing the work.
	{
		TArray<uint8> Garbage;
		Garbage.SetNumZeroed(64);
		FFileHelper::SaveArrayToFile(Garbage, *FilePath);
	}
	FString RecoverError;
	const bool bRecovered = Grid->LoadGameFrom(SlotName, RecoverError);
	TestTrue(TEXT("a corrupt primary file is recovered from the backup"), bRecovered);
	TestTrue(TEXT("the work is intact after recovery"), Grid->GetWidth() == 64 && Grid->GetCell(5, 7) == static_cast<uint8>(3));

	// --- unreadable save must be reported, never fatal ---------------------------------------
	{
		TArray<uint8> Garbage;
		Garbage.SetNumZeroed(4096);
		FFileHelper::SaveArrayToFile(Garbage, *FilePath);
		IFileManager::Get().Delete(*(FilePath + TEXT(".bak")), false, true, true);
	}
	FString CorruptError;
	const bool bCorruptLoaded = Grid->LoadGameFrom(SlotName, CorruptError);
	TestFalse(TEXT("an unreadable save reports failure"), bCorruptLoaded);
	TestTrue(TEXT("the failure carries a readable message"), !CorruptError.IsEmpty());

	// --- clean up ----------------------------------------------------------------------------
	IFileManager::Get().Delete(*FilePath, false, true, true);
	IFileManager::Get().Delete(*(FilePath + TEXT(".bak")), false, true, true);
	IFileManager::Get().Delete(*(FilePath + TEXT(".tmp")), false, true, true);
	Grid->Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// Undo history memory budget
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeanHistoryBudgetTest,
	"Bean.Grid.HistoryStaysBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBeanHistoryBudgetTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddError(TEXT("GEditor is unavailable; this test needs the editor context."));
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddError(TEXT("no editor world available to spawn the grid actor in."));
		return false;
	}

	ABeanGrid* Grid = World->SpawnActor<ABeanGrid>();
	if (!Grid) { AddError(TEXT("could not spawn ABeanGrid.")); return false; }
	Grid->DispatchBeginPlay();

	// A 256x256 canvas makes every full-canvas batch edit a ~65k-entry delta. Repeating that many
	// times must not grow the history without bound.
	Grid->ResizeGrid(256, 256);
	Grid->ClearHistory();

	for (int32 Iteration = 0; Iteration < 200; ++Iteration)
	{
		const bool bPlace = (Iteration % 2) == 0;
		Grid->ApplyBatch(bPlace ? EBeanBatchOp::PlaceCurrentColor : EBeanBatchOp::Remove, 0, 0, 255, 255);
	}

	const int32 UndoSteps = Grid->GetUndoCount();
	AddInfo(FString::Printf(TEXT("history after 200 full-canvas edits: %d undo steps (budget %.0f MB)"),
		UndoSteps, static_cast<double>(ABeanGrid::HistoryMemoryLimitBytes) / (1024.0 * 1024.0)));

	TestTrue(TEXT("history is non-empty"), UndoSteps > 0);
	TestTrue(TEXT("history was trimmed instead of growing to 200 steps"), UndoSteps < 200);

	// Undo must still work after trimming, and must never corrupt a cell into an invalid index.
	for (int32 Step = 0; Step < UndoSteps; ++Step)
	{
		Grid->Undo();
	}
	for (int32 Y = 0; Y < 8; ++Y)
	{
		for (int32 X = 0; X < 8; ++X)
		{
			const uint8 Cell = Grid->GetCell(X, Y);
			TestTrue(TEXT("every cell holds either the palette range or the empty marker"),
				Cell == ABeanGrid::EmptyCell || Cell < 48);
		}
	}

	Grid->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
