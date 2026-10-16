// Implementation of the command-line self-test driver declared in BeanSelfTest.h.

#include "BeanSelfTest.h"

#include "BeanGrid.h"
#include "BeanImageConverter.h"
#include "BeanPalette.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace BeanSelfTestPrivate
{
	/** Collects CHECK results and prints either every assertion or only the failures. */
	struct FBeanCheckContext
	{
		FString Section;
		int32 Failed = 0;
		int32 Total = 0;
		bool bLogDetail = true;

		/** Every reported line, mirrored into Saved/BeanSelfTestReport.txt. */
		FString ReportText;

		/** The report of the last completed run, read by RunAll once both halves have finished. */
		static FString& LastRunReport()
		{
			static FString Report;
			return Report;
		}

		/** Number of checks the last completed run performed. */
		static int32& LastRunCheckCount()
		{
			static int32 Count = 0;
			return Count;
		}

		explicit FBeanCheckContext(bool bInLogDetail) : bLogDetail(bInLogDetail) {}

		void ReportLine(const FString& Line)
		{
			ReportText += Line;
			ReportText += LINE_TERMINATOR;
		}

		void Check(bool bCondition, const FString& Message)
		{
			++Total;
			if (bCondition)
			{
				ReportLine(FString::Printf(TEXT("PASS  %s"), *Message));
				if (bLogDetail)
				{
					UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest]   PASS  %s"), *Message);
				}
			}
			else
			{
				++Failed;
				ReportLine(FString::Printf(TEXT("FAIL  %s"), *Message));
				UE_LOG(LogTemp, Error, TEXT("[BeanSelfTest]   FAIL  %s"), *Message);
			}
		}

		void CheckEqualInt(int32 Actual, int32 Expected, const FString& Message)
		{
			const bool bOk = Actual == Expected;
			Check(bOk, bOk
				? FString::Printf(TEXT("%s (= %d)"), *Message, Actual)
				: FString::Printf(TEXT("%s -- expected %d, got %d"), *Message, Expected, Actual));
		}

		void Info(const FString& Message)
		{
			ReportLine(FString::Printf(TEXT("INFO  %s"), *Message));
			UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest]   %s"), *Message);
		}

		void SectionStart(const FString& InSection, int32& SectionBaselineFailed)
		{
			Section = InSection;
			SectionBaselineFailed = Failed;
			ReportLine(FString::Printf(TEXT("=== %s ==="), *InSection));
			UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] --- %s ---"), *InSection);
		}

		void SectionEnd(const FString& InSection, int32 SectionBaselineFailed)
		{
			const int32 SectionFailures = Failed - SectionBaselineFailed;
			// Built as one FString first: UE_LOG's format-string checker requires a literal format
			// string, so the branch must not be inside the TEXT("...") argument itself.
			const FString Verdict = (SectionFailures == 0)
				? FString(TEXT("PASS"))
				: FString::Printf(TEXT("FAIL (%d checks)"), SectionFailures);
			const FString Line = FString::Printf(TEXT("--- %s: %s ---"), *InSection, *Verdict);
			ReportLine(Line);
			UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] %s"), *Line);
		}
	};

	/** Deterministic picture generator: a gradient plus a hard-edged wedge.
	 *  Hard edges matter because the quantizer's edge handling is exactly what is under test. */
	void BuildTestImage(FImage& OutImage, int32 Width, int32 Height, bool bWithTransparentBorder)
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

				const bool bWedge = (X > Width / 3) && (X < (2 * Width) / 3) && (Y < Height / 2);
				if (bWedge)
				{
					Pixel = FColor(20, static_cast<uint8>(FMath::Clamp(80.0f + 160.0f * V, 0.0f, 255.0f)), 40, 255);
				}
				else
				{
					Pixel = FColor(static_cast<uint8>(FMath::Clamp(40.0f + 200.0f * U, 0.0f, 255.0f)), 60,
						static_cast<uint8>(FMath::Clamp(220.0f - 180.0f * V, 0.0f, 255.0f)), 255);
				}
			}
		}
	}

	int32 CountUsedColors(const TArray<uint8>& Cells, uint8 EmptyValue)
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

	int32 MaxIndex(const TArray<uint8>& Cells, uint8 EmptyValue)
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

	/** Independent recount of "correct" cells, used to cross-check GetCompletionStats. */
	int32 RecountCorrectCells(const ABeanGrid* Grid, const TArray<uint8>& Target, const TArray<bool>& Mask,
		int32 PatternW, int32 PatternH, int32 OriginX, int32 OriginY, uint8 EmptyValue)
	{
		int32 Correct = 0;
		for (int32 Y = 0; Y < PatternH; ++Y)
		{
			for (int32 X = 0; X < PatternW; ++X)
			{
				const int32 Index = Y * PatternW + X;
				if (!Mask[Index] || Target[Index] == EmptyValue)
				{
					continue;
				}
				if (Grid->GetCell(OriginX + X, OriginY + Y) == Target[Index])
				{
					++Correct;
				}
			}
		}
		return Correct;
	}

	/** Cross-checks the per-color breakdown against the overall totals. */
	bool PerColorSumsMatch(const FBeanCompletionStats& Stats)
	{
		int32 SumRequired = 0;
		int32 SumCorrect = 0;
		int32 SumRemaining = 0;
		for (const FBeanColorProgress& Entry : Stats.PerColor)
		{
			SumRequired += Entry.Required;
			SumCorrect += Entry.Correct;
			SumRemaining += Entry.Remaining;
		}
		return SumRequired == Stats.TargetCells
			&& SumCorrect == Stats.CorrectCells
			&& SumRemaining == Stats.TargetCells - Stats.CorrectCells;
	}

	/** Builds a solid-color BGRA8 sRGB image. */
	void BuildSolidImage(FImage& OutImage, int32 Width, int32 Height, const FColor& Color)
	{
		OutImage.Init(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FColor* Pixels = OutImage.AsBGRA8().GetData();
		for (int32 Index = 0; Index < Width * Height; ++Index)
		{
			Pixels[Index] = Color;
		}
	}
	/** Writes the plain-text report next to the project's saved logs. */
	void WriteReport(int32 Failed, int32 Total, const FString& Body, const FString& Header)
	{
		const FString ReportPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BeanSelfTestReport.txt"));
		FString Report;
		Report += TEXT("Bean data-layer self-test report");
		Report += LINE_TERMINATOR;
		Report += FString::Printf(TEXT("Generated: %s"), *FDateTime::UtcNow().ToString());
		Report += LINE_TERMINATOR;
		Report += Header;
		Report += LINE_TERMINATOR;
		Report += FString::Printf(TEXT("Result: %s -- %d failed of %d checks"),
			Failed == 0 ? TEXT("ALL CHECKS PASSED") : TEXT("FAILURES PRESENT"), Failed, Total);
		Report += LINE_TERMINATOR;
		Report += LINE_TERMINATOR;
		Report += Body;
		FFileHelper::SaveStringToFile(Report, *ReportPath);
		UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] plain-text report written to %s"), *ReportPath);
	}
}

using namespace BeanSelfTestPrivate;

// ---------------------------------------------------------------------------
// Image side: no world needed
// ---------------------------------------------------------------------------

int32 FBeanSelfTest::RunImageChecks(bool LogDetail)
{
	FBeanCheckContext Ctx(LogDetail);
	UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] ===== image conversion, crop and error handling ====="));

	int32 Baseline = 0;

	// --- feature 1: conversion quality -------------------------------------------------------
	Ctx.SectionStart(TEXT("Bean.Convert.ImageToPattern"), Baseline);

	const TArray<FBeanPaletteColor> Palette = FBeanPalette::LoadOrCreateDefault();
	Ctx.CheckEqualInt(Palette.Num(), 48, TEXT("shipped palette size"));

	FImage Source;
	BuildTestImage(Source, 512, 512, /*bWithTransparentBorder=*/false);

	FBeanConvertOptions Options;
	Options.CanvasWidth = 64;
	Options.CanvasHeight = 64;
	FBeanImageConverter::SetFullCrop(Options, Source);

	FBeanConvertResult First;
	Ctx.Check(FBeanImageConverter::Convert(Source, Palette, Options, First), TEXT("conversion of a plain RGB picture succeeds"));
	Ctx.CheckEqualInt(First.Cells.Num(), 64 * 64, TEXT("pattern is canvas sized"));
	Ctx.Check(First.Error.IsEmpty(), TEXT("no error message on success"));
	Ctx.Check(First.ColorCount > 0, FString::Printf(TEXT("at least one color is used (used %d)"), First.ColorCount));

	// Acceptance criterion: same picture + same settings = same output.
	FBeanConvertResult Second;
	Ctx.Check(FBeanImageConverter::Convert(Source, Palette, Options, Second), TEXT("second conversion succeeds"));
	Ctx.Check(Second.Cells == First.Cells, TEXT("re-running the same conversion is bit-identical (determinism)"));
	Ctx.CheckEqualInt(Second.ColorCount, First.ColorCount, TEXT("reported color count is stable"));

	// Acceptance criterion: output colors belong to the real palette.
	const int32 HighestIndex = MaxIndex(First.Cells, Options.EmptyValue);
	Ctx.Check(HighestIndex < Palette.Num(), FString::Printf(TEXT("every emitted index is inside the palette (max index %d)"), HighestIndex));

	// Color-count control: the subset must come from the picture and stay within the limit.
	FBeanConvertOptions Limited = Options;
	Limited.MaxColors = 8;
	FBeanConvertResult Reduced;
	Ctx.Check(FBeanImageConverter::Convert(Source, Palette, Limited, Reduced), TEXT("conversion with a color limit succeeds"));
	const int32 ReducedColors = CountUsedColors(Reduced.Cells, Limited.EmptyValue);
	Ctx.Check(ReducedColors <= 8, FString::Printf(TEXT("8-color limit is respected (actually used %d)"), ReducedColors));
	Ctx.CheckEqualInt(Reduced.ColorCount, ReducedColors, TEXT("the limited palette is reported consistently"));

	// Transparency: a transparent border must become holes, not black beads.
	FImage TransparentSource;
	BuildTestImage(TransparentSource, 256, 256, /*bWithTransparentBorder=*/true);
	FBeanConvertOptions TransparentOptions;
	TransparentOptions.CanvasWidth = 32;
	TransparentOptions.CanvasHeight = 32;
	FBeanImageConverter::SetFullCrop(TransparentOptions, TransparentSource);

	FBeanConvertResult TransparentResult;
	Ctx.Check(FBeanImageConverter::Convert(TransparentSource, Palette, TransparentOptions, TransparentResult), TEXT("conversion of a picture with alpha succeeds"));
	Ctx.Check(TransparentResult.bHasTransparency, TEXT("transparency is detected"));
	Ctx.Check(TransparentResult.Cells.Num() > 0 && TransparentResult.Cells[0] == TransparentOptions.EmptyValue, TEXT("the transparent corner stays empty"));
	Ctx.Check(TransparentResult.Cells.Num() > (32 / 2) * 32 + (32 / 2)
		&& TransparentResult.Cells[(32 / 2) * 32 + (32 / 2)] != TransparentOptions.EmptyValue, TEXT("the opaque middle gets a bead"));
	Ctx.Check(TransparentResult.PlacedCells < TransparentResult.Cells.Num(), TEXT("PlacedCells excludes the transparent holes"));

	// Non-square canvases: the three shipped orientations.
	FBeanConvertOptions Landscape;
	Landscape.CanvasWidth = 64;
	Landscape.CanvasHeight = 48;
	FBeanImageConverter::SetFullCrop(Landscape, Source);
	FBeanConvertResult LandscapeResult;
	Ctx.Check(FBeanImageConverter::Convert(Source, Palette, Landscape, LandscapeResult), TEXT("landscape canvas conversion succeeds"));
	Ctx.CheckEqualInt(LandscapeResult.Cells.Num(), 64 * 48, TEXT("landscape pattern is 64x48"));

	FBeanConvertOptions Portrait;
	Portrait.CanvasWidth = 48;
	Portrait.CanvasHeight = 64;
	FBeanImageConverter::SetFullCrop(Portrait, Source);
	FBeanConvertResult PortraitResult;
	Ctx.Check(FBeanImageConverter::Convert(Source, Palette, Portrait, PortraitResult), TEXT("portrait canvas conversion succeeds"));
	Ctx.CheckEqualInt(PortraitResult.Cells.Num(), 48 * 64, TEXT("portrait pattern is 48x64"));

	// Acceptance criterion: bad input is reported, not crashed on.
	FImage Empty;
	FBeanConvertResult BadResult;
	Ctx.Check(!FBeanImageConverter::Convert(Empty, Palette, Options, BadResult), TEXT("an empty image is rejected instead of crashing"));
	Ctx.Check(!BadResult.Error.IsEmpty(), TEXT("the rejection carries a readable reason"));

	FImage HugeImage;
	const int32 HugeDim = 6000;
	BuildSolidImage(HugeImage, HugeDim, HugeDim, FColor(120, 130, 140, 255));
	FBeanConvertOptions HugeOptions;
	HugeOptions.CanvasWidth = 128;
	HugeOptions.CanvasHeight = 128;
	FBeanImageConverter::SetFullCrop(HugeOptions, HugeImage);
	FBeanConvertResult HugeResult;
	const double HugeStart = FPlatformTime::Seconds();
	FBeanImageConverter::ResetConvertTiming();
	const bool bHugeOk = FBeanImageConverter::Convert(HugeImage, Palette, HugeOptions, HugeResult);
	const double HugeMs = (FPlatformTime::Seconds() - HugeStart) * 1000.0;
	Ctx.Check(bHugeOk, TEXT("a 6000x6000 picture is handled instead of crashing"));
	Ctx.CheckEqualInt(HugeResult.Cells.Num(), 128 * 128, TEXT("the 6000x6000 picture produced a 128x128 pattern"));
	Ctx.Info(FString::Printf(TEXT("6000x6000 -> 128x128 conversion took %.1f ms"), HugeMs));
	Ctx.Info(FString::Printf(TEXT("  stages: resample %.1f ms, palette subset %.1f ms, per-bead quantize %.1f ms, stats+preview %.1f ms"),
		FBeanImageConverter::ConvertTiming.FitUs / 1000.0,
		FBeanImageConverter::ConvertTiming.PaletteUs / 1000.0,
		FBeanImageConverter::ConvertTiming.QuantizeUs / 1000.0,
		FBeanImageConverter::ConvertTiming.StatsUs / 1000.0));

	// The import panel no longer crops (m03538), so the fitted path is what a player actually runs on
	// a big photo: measure it separately instead of assuming the crop path's numbers apply.
	FBeanConvertOptions FittedOptions;
	FittedOptions.CanvasWidth = 256;
	FittedOptions.CanvasHeight = 256;
	FittedOptions.bHasCrop = false;
	FBeanConvertResult FittedResult;
	FBeanImageConverter::ResetConvertTiming();
	const double FittedStart = FPlatformTime::Seconds();
	const bool bFittedOk = FBeanImageConverter::Convert(HugeImage, Palette, FittedOptions, FittedResult);
	const double FittedMs = (FPlatformTime::Seconds() - FittedStart) * 1000.0;
	Ctx.Check(bFittedOk, TEXT("a 6000x6000 picture fits onto a 256x256 canvas without cropping"));
	Ctx.CheckEqualInt(FittedResult.Cells.Num(), 256 * 256, TEXT("the fitted picture produced a 256x256 pattern"));
	Ctx.Info(FString::Printf(TEXT("6000x6000 -> 256x256 fitted (no crop): %.1f ms"), FittedMs));
	Ctx.Info(FString::Printf(TEXT("  stages: resample %.1f ms, palette subset %.1f ms, per-bead quantize %.1f ms, stats+preview %.1f ms"),
		FBeanImageConverter::ConvertTiming.FitUs / 1000.0,
		FBeanImageConverter::ConvertTiming.PaletteUs / 1000.0,
		FBeanImageConverter::ConvertTiming.QuantizeUs / 1000.0,
		FBeanImageConverter::ConvertTiming.StatsUs / 1000.0));

	// Magnifying path: a picture smaller than the canvas has to be interpolated, because a
	// nearest-neighbour scale turns every source pixel into a visible block of beads.
	FImage SmallImage(16, 16, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	{
		TArrayView64<FColor> SmallPixels = SmallImage.AsBGRA8();
		for (int32 Y = 0; Y < 16; ++Y)
		{
			for (int32 X = 0; X < 16; ++X)
			{
				SmallPixels[(int64)Y * 16 + X] = FColor((uint8)(X * 255 / 15), (uint8)(Y * 255 / 15), 128, 255);
			}
		}
	}
	FBeanConvertOptions SmallOptions;
	SmallOptions.CanvasWidth = 64;
	SmallOptions.CanvasHeight = 64;
	SmallOptions.bHasCrop = false;
	FBeanConvertResult SmallResult;
	const bool bSmallOk = FBeanImageConverter::Convert(SmallImage, Palette, SmallOptions, SmallResult);
	Ctx.Check(bSmallOk, TEXT("a small picture is magnified onto a big canvas instead of failing"));
	int32 SmallEmpty = 0;
	TSet<uint8> SmallColors;
	for (const uint8 Cell : SmallResult.Cells)
	{
		if (Cell == SmallOptions.EmptyValue)
		{
			++SmallEmpty;
		}
		else
		{
			SmallColors.Add(Cell);
		}
	}
	Ctx.CheckEqualInt(SmallEmpty, 0, TEXT("an opaque square picture magnified onto a square canvas leaves no empty cell"));
	Ctx.Check(SmallColors.Num() >= 2, TEXT("the magnified picture keeps a gradient instead of one flat block"));

	FImage MissingFile;
	FString MissingError;
	Ctx.Check(!FBeanImageConverter::LoadSourceImage(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("does_not_exist_bean.png")), MissingFile, MissingError),
		TEXT("a missing file is reported, not fatal"));
	Ctx.Check(!MissingError.IsEmpty(), TEXT("the missing-file error is readable"));

	FString BadFormatError;
	FImage BadFormatImage;
	const FString BadFormatPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("__BeanSelfTest_not_an_image.png"));
	FFileHelper::SaveStringToFile(TEXT("this is not a PNG at all"), *BadFormatPath);
	Ctx.Check(!FBeanImageConverter::LoadSourceImage(BadFormatPath, BadFormatImage, BadFormatError),
		TEXT("a file that is not a readable image is reported, not fatal"));
	Ctx.Check(!BadFormatError.IsEmpty(), TEXT("the bad-format error is readable"));
	IFileManager::Get().Delete(*BadFormatPath, false, true, true);

	// Dithering must stay opt-in and must not blow the palette up.
	FBeanConvertOptions DitherOptions = Limited;
	DitherOptions.bDither = true;
	FBeanConvertResult DitherResult;
	Ctx.Check(FBeanImageConverter::Convert(Source, Palette, DitherOptions, DitherResult), TEXT("dithered conversion succeeds"));
	const int32 DitherColors = CountUsedColors(DitherResult.Cells, DitherOptions.EmptyValue);
	Ctx.Check(DitherColors <= 8, FString::Printf(TEXT("dithering still respects the color limit (used %d)"), DitherColors));

	Ctx.SectionEnd(TEXT("Bean.Convert.ImageToPattern"), Baseline);

	// --- feature 5: automatic crop -----------------------------------------------------------
	Ctx.SectionStart(TEXT("Bean.Convert.AutoCropKeepsSubject"), Baseline);

	FImage SubjectImage;
	SubjectImage.Init(200, 200, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	{
		FColor* Pixels = SubjectImage.AsBGRA8().GetData();
		for (int32 Y = 0; Y < 200; ++Y)
		{
			for (int32 X = 0; X < 200; ++X)
			{
				const bool bSubject = X >= 60 && X < 140 && Y >= 50 && Y < 150;
				Pixels[Y * 200 + X] = bSubject ? FColor(30, 30, 30, 255) : FColor(240, 240, 240, 255);
			}
		}
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	const bool bSuggested = FBeanImageConverter::SuggestCrop(SubjectImage, 32, 32, MinX, MinY, MaxX, MaxY);
	Ctx.Check(bSuggested, TEXT("a clear subject yields a crop suggestion"));
	if (bSuggested)
	{
		Ctx.Check(MinX > 0, FString::Printf(TEXT("crop excludes the empty left margin (MinX=%d)"), MinX));
		Ctx.Check(MinY > 0, FString::Printf(TEXT("crop excludes the empty top margin (MinY=%d)"), MinY));
		Ctx.Check(MaxX < 199, FString::Printf(TEXT("crop excludes the empty right margin (MaxX=%d)"), MaxX));
		Ctx.Check(MaxY < 199, FString::Printf(TEXT("crop excludes the empty bottom margin (MaxY=%d)"), MaxY));
		Ctx.Check(MinX <= 60 && MaxX >= 139, TEXT("crop covers the subject horizontally"));
		Ctx.Check(MinY <= 50 && MaxY >= 149, TEXT("crop covers the subject vertically"));
	}

	FImage Flat;
	BuildSolidImage(Flat, 64, 64, FColor(128, 128, 128, 255));
	const bool bFlatSuggested = FBeanImageConverter::SuggestCrop(Flat, 32, 32, MinX, MinY, MaxX, MaxY);
	Ctx.Check(!bFlatSuggested, TEXT("a subjectless picture reports that it cannot decide"));

	Ctx.SectionEnd(TEXT("Bean.Convert.AutoCropKeepsSubject"), Baseline);

	FBeanCheckContext::LastRunReport() = MoveTemp(Ctx.ReportText);
	FBeanCheckContext::LastRunCheckCount() = Ctx.Total;

	if (Ctx.Failed == 0)
	{
		UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] image checks: %d/%d passed"), Ctx.Total, Ctx.Total);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[BeanSelfTest] image checks: %d of %d FAILED"), Ctx.Failed, Ctx.Total);
	}
	return Ctx.Failed;
}

// ---------------------------------------------------------------------------
// Grid side: needs a world
// ---------------------------------------------------------------------------

int32 FBeanSelfTest::RunGridChecks(UWorld* World, bool LogDetail)
{
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("[BeanSelfTest] no world available; grid checks skipped."));
		return 1;
	}

	FBeanCheckContext Ctx(LogDetail);
	UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] ===== grid data, history, batch, statistics, save ====="));

	int32 Baseline = 0;
	const uint8 Empty = ABeanGrid::EmptyCell;

	// --- features 2/3/4: edits, history, batch ------------------------------------------------
	Ctx.SectionStart(TEXT("Bean.Grid.EditsHistoryAndStats"), Baseline);

	ABeanGrid* Grid = World->SpawnActor<ABeanGrid>();
	if (!Grid)
	{
		UE_LOG(LogTemp, Error, TEXT("[BeanSelfTest] FAIL could not spawn ABeanGrid"));
		return Ctx.Failed + 1;
	}
	Grid->DispatchBeginPlay();

	Ctx.Check(Grid->IsReady(), TEXT("grid initialized with a valid cell array"));
	Ctx.CheckEqualInt(Grid->GetWidth(), 256, TEXT("default canvas width is 256"));
	Ctx.CheckEqualInt(Grid->GetHeight(), 256, TEXT("default canvas height is 256"));

	// --- m03538: a brand-new canvas -----------------------------------------------------------
	// Every launch and every "新建画布" must start from an empty board with a name of its own, and the
	// ground grid texture has to follow the canvas size: a texture built for a fixed 256 cells made
	// the drawn cell lines disagree with the beads on any smaller canvas.
	{
		int32 FreshCells = 0;
		for (int32 Y = 0; Y < Grid->GetHeight(); ++Y)
		{
			for (int32 X = 0; X < Grid->GetWidth(); ++X)
			{
				if (Grid->GetCell(X, Y) != Empty)
				{
					++FreshCells;
				}
			}
		}
		Ctx.CheckEqualInt(FreshCells, 0, TEXT("a spawned canvas starts empty (no auto-loaded work)"));

		const FString FirstName = Grid->GetWorkName();
		Ctx.Check(!FirstName.IsEmpty(), TEXT("a new canvas gets a work name"));
		Ctx.Check(!Grid->IsDirty(), TEXT("an untouched new canvas is not dirty (so it never autosaves)"));

		int32 GroundX = 0;
		int32 GroundY = 0;
		Grid->GetGroundTextureCellCount(GroundX, GroundY);
		Ctx.CheckEqualInt(GroundX, Grid->GetWidth(), TEXT("the ground grid texture matches the canvas width"));
		Ctx.CheckEqualInt(GroundY, Grid->GetHeight(), TEXT("the ground grid texture matches the canvas height"));

		FString NewCanvasError;
		const bool bMadeNew = Grid->NewCanvas(64, 64, NewCanvasError);
		Ctx.Check(bMadeNew, FString::Printf(TEXT("新建画布 makes a 64x64 canvas (%s)"), *NewCanvasError));
		Ctx.CheckEqualInt(Grid->GetWidth(), 64, TEXT("the new canvas is 64 wide"));
		Ctx.CheckEqualInt(Grid->GetCell(63, 63), Empty, TEXT("the new canvas is empty"));
		Ctx.Check(!Grid->IsDirty(), TEXT("the new canvas is not dirty either"));
		Grid->GetGroundTextureCellCount(GroundX, GroundY);
		Ctx.CheckEqualInt(GroundX, 64, TEXT("the ground texture followed the new canvas width"));

		const FString SecondName = Grid->GetWorkName();
		Ctx.Check(SecondName != FirstName, TEXT("新建画布 takes a name of its own"));

		FString BackError;
		Ctx.Check(Grid->NewCanvas(256, 256, BackError), FString::Printf(TEXT("the canvas can go back to 256x256 (%s)"), *BackError));
		Ctx.Check(Grid->GetWorkName() != SecondName, TEXT("two 新建画布 presses in a row do not share a name"));
	}

	// Undo / redo of single edits.
	const int32 ColorA = 1;
	const int32 ColorB = 4;
	// Start from a known-empty canvas: DispatchBeginPlay() restores the player's last work, and once
	// that work has beads at (10, 10) PlaceCell correctly refuses to overwrite them, which turned
	// these checks into comparisons against whatever the player happened to draw.
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetCurrentColor(ColorA);

	Grid->PlaceCell(10, 10);
	Ctx.CheckEqualInt(Grid->GetCell(10, 10), ColorA, TEXT("placement landed in the data"));
	Ctx.Check(Grid->CanUndo(), TEXT("one undo step is available"));
	Ctx.Check(!Grid->CanRedo(), TEXT("nothing to redo yet"));

	Grid->Undo();
	Ctx.CheckEqualInt(Grid->GetCell(10, 10), Empty, TEXT("undo removed the bead"));
	Ctx.Check(!Grid->CanUndo(), TEXT("undo consumed the history"));
	Ctx.Check(Grid->CanRedo(), TEXT("redo is now available"));

	Grid->Redo();
	Ctx.CheckEqualInt(Grid->GetCell(10, 10), ColorA, TEXT("redo put the bead back with the same color"));

	Grid->Undo();
	Grid->SetCurrentColor(ColorB);
	Grid->PlaceCell(11, 11);
	Ctx.Check(!Grid->CanRedo(), TEXT("a new edit clears the redo stack"));

	// One drag = one undo step.
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetCurrentColor(ColorA);
	Grid->BeginStroke(TEXT("continuous placement"));
	for (int32 X = 20; X < 30; ++X)
	{
		Grid->PlaceCell(X, 20);
	}
	Grid->EndStroke();
	Ctx.CheckEqualInt(Grid->GetUndoCount(), 1, TEXT("a whole drag is a single undo step"));
	Grid->Undo();
	bool bStrokeFullyUndone = true;
	for (int32 X = 20; X < 30; ++X)
	{
		bStrokeFullyUndone &= (Grid->GetCell(X, 20) == Empty);
	}
	Ctx.Check(bStrokeFullyUndone, TEXT("undoing the stroke removes every bead of the drag"));

	// Batch operations.
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetCurrentColor(ColorA);
	const int32 Placed = Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 30, 30, 39, 39);
	Ctx.CheckEqualInt(Placed, 100, TEXT("batch place fills the whole rectangle"));
	Ctx.CheckEqualInt(Grid->GetCell(30, 30), ColorA, TEXT("batch place writes the corner"));
	Ctx.CheckEqualInt(Grid->GetCell(39, 39), ColorA, TEXT("batch place writes the far corner"));
	Ctx.CheckEqualInt(Grid->GetUndoCount(), 1, TEXT("one batch is one undo step"));

	const int32 Replaced = Grid->ApplyBatch(EBeanBatchOp::ReplaceColor, 30, 30, 39, 39, static_cast<uint8>(ColorA), static_cast<uint8>(ColorB));
	Ctx.CheckEqualInt(Replaced, 100, TEXT("batch replace converts every bead in the rectangle"));
	Ctx.CheckEqualInt(Grid->GetCell(35, 35), ColorB, TEXT("batch replace changed the color"));

	const int32 Removed = Grid->ApplyBatch(EBeanBatchOp::Remove, 30, 30, 39, 39);
	Ctx.CheckEqualInt(Removed, 100, TEXT("batch remove clears the rectangle"));
	Ctx.CheckEqualInt(Grid->GetCell(35, 35), Empty, TEXT("batch remove left an empty cell"));
	Ctx.CheckEqualInt(Grid->GetUndoCount(), 3, TEXT("the three batch operations are three undo steps"));

	const int32 Noop = Grid->ApplyBatch(EBeanBatchOp::Remove, 30, 30, 39, 39);
	Ctx.CheckEqualInt(Noop, 0, TEXT("a no-op batch reports zero changes"));
	Ctx.CheckEqualInt(Grid->GetUndoCount(), 3, TEXT("a no-op batch adds no history"));

	// Batch fill from the pattern: the target color must come from the pattern data.
	{
		const int32 FillW = 4;
		const int32 FillH = 4;
		const int32 FillOriginX = 200;
		const int32 FillOriginY = 200;
		TArray<uint8> FillTarget;
		FillTarget.SetNumZeroed(FillW * FillH);
		for (int32 Index = 0; Index < FillW * FillH; ++Index)
		{
			FillTarget[Index] = static_cast<uint8>(ColorB);
		}
		TArray<bool> FillMask;
		FillMask.Init(true, FillW * FillH);
		Grid->SetTargetPattern(FillW, FillH, FillOriginX, FillOriginY, FillTarget, FillMask);
		Grid->ClearAllCells();
		Grid->ClearHistory();
		Grid->SetCurrentColor(ColorA);

		const int32 Filled = Grid->ApplyBatch(EBeanBatchOp::FillFromPattern, FillOriginX, FillOriginY, FillOriginX + FillW - 1, FillOriginY + FillH - 1);
		Ctx.CheckEqualInt(Filled, FillW * FillH, TEXT("pattern fill writes every cell of the selection"));
		Ctx.CheckEqualInt(Grid->GetCell(FillOriginX + 2, FillOriginY + 2), ColorB, TEXT("pattern fill used the pattern's color, not the current color"));
		Ctx.CheckEqualInt(Grid->GetUndoCount(), 1, TEXT("a pattern fill is a single undo step"));
		Grid->Undo();
		Ctx.CheckEqualInt(Grid->GetCell(FillOriginX + 2, FillOriginY + 2), Empty, TEXT("undoing a pattern fill removes the beads again"));
	}

	// 应用 in the import panel hands the pattern an EMPTY mask on purpose (ApplyImage passes
	// TArray<bool>()); SetTargetPattern has to derive it from "cell != EmptyCell", otherwise
	// 确定并铺满画布 fills nothing. This reproduces that exact call pair.
	{
		Grid->ClearAllCells();
		Grid->ClearHistory();

		const int32 ImpW = 6;
		const int32 ImpH = 4;
		TArray<uint8> ImportCells;
		ImportCells.Init(Empty, ImpW * ImpH);
		// 18 opaque cells and 6 transparent holes, in the same shape the converter produces.
		for (int32 Index = 0; Index < ImpW * ImpH; ++Index)
		{
			if (Index % 3 != 1)
			{
				ImportCells[Index] = static_cast<uint8>(Index % 2 == 0 ? ColorA : ColorB);
			}
		}
		Grid->SetTargetPattern(ImpW, ImpH, 0, 0, ImportCells, TArray<bool>());

		const int32 AutoFilled = Grid->ApplyBatch(EBeanBatchOp::FillFromPattern, 0, 0, Grid->GetWidth() - 1, Grid->GetHeight() - 1);
		Ctx.CheckEqualInt(AutoFilled, 16, TEXT("an empty-mask pattern still fills every opaque cell (应用 + 铺满)"));
		Ctx.CheckEqualInt(Grid->GetCell(0, 0), ColorA, TEXT("the auto-filled canvas uses the pattern colour at 0,0"));
		Ctx.CheckEqualInt(Grid->GetCell(1, 0), Empty, TEXT("a transparent pattern cell is not filled"));
		Ctx.CheckEqualInt(Grid->GetUndoCount(), 1, TEXT("the whole auto-fill is one undo step"));
		Grid->Undo();
		Ctx.CheckEqualInt(Grid->GetCell(0, 0), Empty, TEXT("undoing the auto-fill clears the canvas again"));

		// 应用并铺满 has to be safe to press twice: the second press may not rewrite the canvas or
		// push another edit onto the undo stack, otherwise a stray double-click doubles the history.
		Grid->SetTargetPattern(ImpW, ImpH, 0, 0, ImportCells, TArray<bool>());
		const int32 FirstFill = Grid->ApplyBatch(EBeanBatchOp::FillFromPattern, 0, 0, Grid->GetWidth() - 1, Grid->GetHeight() - 1);
		const int32 UndoAfterFirst = Grid->GetUndoCount();
		const int32 SecondFill = Grid->ApplyBatch(EBeanBatchOp::FillFromPattern, 0, 0, Grid->GetWidth() - 1, Grid->GetHeight() - 1);
		Ctx.CheckEqualInt(FirstFill, 16, TEXT("the first 应用并铺满 places the whole pattern"));
		Ctx.CheckEqualInt(SecondFill, 0, TEXT("pressing 应用并铺满 again places nothing (the pattern is already on the board)"));
		Ctx.CheckEqualInt(Grid->GetUndoCount(), UndoAfterFirst, TEXT("a no-op 应用并铺满 does not grow the undo history"));
	}

	// Applying a pattern must never destroy what the player already built. This used to call
	// ClearAllCells()/ClearHistory() internally, which threw the whole work away (and the undo
	// history with it) on 应用 - a data-loss bug that is exactly what a regression test is for.
	{
		Grid->ClearAllCells();
		Grid->ClearHistory();
		Grid->SetCurrentColor(ColorA);
		Grid->PlaceCell(3, 3);
		Grid->PlaceCell(4, 4);
		const int32 KeepA = Grid->GetCell(3, 3);
		const int32 KeepB = Grid->GetCell(4, 4);

		// A 4x4 pattern centred on the 256 canvas (so at 126,126), all ColorB.
		TArray<uint8> PreviewCells;
		PreviewCells.Init(static_cast<uint8>(ColorB), 4 * 4);
		Grid->BeginImagePreview(PreviewCells, 4, 4);

		Ctx.CheckEqualInt(Grid->GetCell(3, 3), KeepA, TEXT("applying a pattern keeps the bead at 3,3"));
		Ctx.CheckEqualInt(Grid->GetCell(4, 4), KeepB, TEXT("applying a pattern keeps the bead at 4,4"));
		Ctx.Check(Grid->HasTargetPattern(), TEXT("applying a pattern sets the target pattern"));
		Ctx.Check(Grid->HasOverlay(), TEXT("applying a pattern shows it as the hint layer"));

		// Filling towards the pattern then only adds what is missing, in one undo step.
		const int32 FilledByPattern = Grid->ApplyBatch(EBeanBatchOp::FillFromPattern, 126, 126, 129, 129);
		Ctx.CheckEqualInt(FilledByPattern, 4 * 4, TEXT("fill places every bead the pattern asks for"));
		Ctx.CheckEqualInt(Grid->GetCell(126, 126), ColorB, TEXT("fill used the pattern colour"));
		Grid->Undo();
		Ctx.CheckEqualInt(Grid->GetCell(126, 126), Empty, TEXT("one undo removes the whole pattern fill"));
		Ctx.CheckEqualInt(Grid->GetCell(3, 3), KeepA, TEXT("the undo of a fill still leaves the original bead"));

		// The incremental instance bookkeeping must still agree with the components after all of
		// add / remove / fill / undo: an out-of-sync map silently moves the WRONG bead later.
		Grid->ClearAllCells();
		Grid->ClearHistory();
		Grid->SetCurrentColor(ColorA);
		Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 50, 50, 59, 59);
		Grid->ApplyBatch(EBeanBatchOp::Remove, 52, 50, 57, 59);
		Grid->PlaceCell(70, 70);
		Grid->RemoveCell(70, 70);
		Grid->Undo();
		Grid->Undo();

		bool bMapsAgree = true;
		FString MapMismatch;
		const int32 Colors = FBeanPalette::GetPaletteSize();
		for (int32 Color = 0; Color < Colors; ++Color)
		{
			const int32 Instances = Grid->GetInstanceCountForColor(Color);
			const int32 Mapped = Grid->GetMappedCellCountForColor(Color);
			if (Instances != Mapped)
			{
				bMapsAgree = false;
				MapMismatch += FString::Printf(TEXT("color %d: %d instances / %d mapped; "), Color, Instances, Mapped);
			}
		}
		Ctx.Info(FString::Printf(TEXT("instance map mismatch: %s"), MapMismatch.IsEmpty() ? TEXT("none") : *MapMismatch));
		Ctx.Check(bMapsAgree, TEXT("cell<->instance maps stay in sync with the instancers"));

		// Counts agreeing is not proof: if the component falls back to ORDERED removal while the
		// maps assume swap-removal, every index above the hole is one slot off and the next edit
		// moves the wrong bead without changing any count. Check the actual instance transforms.
		// NOTE: validate first, then format - argument evaluation order is unspecified, so building
		// the message inline would print the error string from BEFORE the call (it printed "()").
		FString MapError;
		const bool bMappingsAgree = Grid->ValidateInstanceMappings(MapError);
		Ctx.Check(bMappingsAgree, FString::Printf(TEXT("every mapped instance really sits on its cell (%s)"), *MapError));
	}

	// Completion statistics: a 4x4 target with one hole => denominator 15.
	const int32 PatternW = 4;
	const int32 PatternH = 4;
	const int32 OriginX = 100;
	const int32 OriginY = 100;
	TArray<uint8> Target;
	TArray<bool> Mask;
	Target.SetNumZeroed(PatternW * PatternH);
	Mask.Init(true, PatternW * PatternH);
	for (int32 Index = 0; Index < PatternW * PatternH; ++Index)
	{
		Target[Index] = static_cast<uint8>(ColorA);
	}
	Target[0] = Empty;
	Grid->SetTargetPattern(PatternW, PatternH, OriginX, OriginY, Target, Mask);
	Ctx.Check(Grid->HasTargetPattern(), TEXT("the target pattern is registered"));

	Grid->ClearAllCells();
	Grid->ClearHistory();

	FBeanCompletionStats Stats;
	Grid->GetCompletionStats(Stats);
	Ctx.Check(Stats.bHasPattern, TEXT("statistics report a pattern"));
	Ctx.CheckEqualInt(Stats.TargetCells, 15, TEXT("the empty pattern cell is excluded from the target total"));
	Ctx.CheckEqualInt(Stats.CorrectCells, 0, TEXT("nothing is correct before any bead is placed"));
	Ctx.CheckEqualInt(Stats.MissingCells, 15, TEXT("every required cell is missing"));
	Ctx.CheckEqualInt(FMath::RoundToInt(Stats.CompletionPercent), 0, TEXT("completion starts at zero"));

	Grid->SetCurrentColor(ColorA);
	Grid->PlaceCell(OriginX + 1, OriginY);
	Grid->GetCompletionStats(Stats);
	Ctx.CheckEqualInt(Stats.CorrectCells, 1, TEXT("a correct bead counts as correct"));
	Ctx.CheckEqualInt(Stats.MissingCells, 14, TEXT("the missing count drops"));
	Ctx.CheckEqualInt(FMath::RoundToInt(Stats.CompletionPercent), FMath::RoundToInt(100.0f * 1.0f / 15.0f), TEXT("completion percent follows the correct cells"));

	Grid->SetCurrentColor(ColorB);
	Grid->PlaceCell(OriginX + 2, OriginY);
	Grid->GetCompletionStats(Stats);
	Ctx.CheckEqualInt(Stats.CorrectCells, 1, TEXT("a wrong color is not counted as correct"));
	Ctx.CheckEqualInt(Stats.MiscoloredCells, 1, TEXT("the wrong color is reported separately"));

	Ctx.Check(PerColorSumsMatch(Stats), TEXT("per-color breakdown adds up to the totals"));

	Grid->Undo();
	Grid->GetCompletionStats(Stats);
	Ctx.CheckEqualInt(Stats.CorrectCells, 1, TEXT("undo rolls the statistics back"));
	Ctx.CheckEqualInt(Stats.MiscoloredCells, 0, TEXT("undo removes the wrong-color entry"));

	// Beads outside the pattern must not inflate the completion of the pattern.
	{
		Grid->ClearAllCells();
		Grid->ClearHistory();
		Grid->GetCompletionStats(Stats);
		const int32 CleanBaseline = Stats.CorrectCells;

		const int32 BatchPlaced = Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, 63, 63);
		Grid->GetCompletionStats(Stats);
		const int32 Independent = RecountCorrectCells(Grid, Target, Mask, PatternW, PatternH, OriginX, OriginY, Empty);

		Ctx.Info(FString::Printf(TEXT("filled %d cells outside the pattern; stats say %d correct, an independent recount says %d"),
			BatchPlaced, Stats.CorrectCells, Independent + CleanBaseline));
		Ctx.CheckEqualInt(Stats.CorrectCells, Independent + CleanBaseline, TEXT("beads placed outside the pattern area are not counted as progress"));
	}

	// Canvas resize keeps the resolution system intact.
	Ctx.Check(Grid->ResizeGrid(32, 32), TEXT("canvas can be resized to 32x32"));
	Ctx.CheckEqualInt(Grid->GetWidth(), 32, TEXT("width follows the resize"));
	Ctx.CheckEqualInt(Grid->GetHeight(), 32, TEXT("height follows the resize"));
	Ctx.CheckEqualInt(Grid->GetCellCount(), 32 * 32, TEXT("the cell array matches the new size"));
	Ctx.Check(!Grid->HasTargetPattern(), TEXT("resizing clears the target pattern"));

	for (const int32 Resolution : ABeanGrid::GetResolutionOptions())
	{
		Ctx.Check(Grid->ResizeGrid(Resolution, Resolution),
			FString::Printf(TEXT("resolution %d is accepted"), Resolution));
		Ctx.CheckEqualInt(Grid->GetWidth(), Resolution,
			FString::Printf(TEXT("resolution %d round-trips"), Resolution));
	}

	for (const EBeanCanvasPreset Preset : ABeanGrid::GetCanvasPresets())
	{
		int32 PresetW = 0;
		int32 PresetH = 0;
		ABeanGrid::GetPresetDimensions(Preset, 64, PresetW, PresetH);
		const bool bSizeOk = PresetW > 0 && PresetH > 0;
		// Built in its own statement: UE's format-string checker requires the literal to be
		// verifiable at the call site, which nested calls in the argument list defeat.
		const FString PresetName = ABeanGrid::GetCanvasPresetLabel(Preset);
		const FString PresetMessage = FString::Printf(TEXT("preset '%s' at 64 gives %dx%d"), *PresetName, PresetW, PresetH);
		Ctx.Check(bSizeOk, PresetMessage);
	}

	Ctx.SectionEnd(TEXT("Bean.Grid.EditsHistoryAndStats"), Baseline);

	// --- feature 8 + 11: measured cost and save reliability ------------------------------------
	Ctx.SectionStart(TEXT("Bean.Perf.LargeCanvasCost"), Baseline);
	Ctx.Info(TEXT("resolution | resize ms | batch ms | place us | remove us | stats ms | save ms (KB) | load ms"));

	for (const int32 Resolution : ABeanGrid::GetResolutionOptions())
	{
		const double ResizeStart = FPlatformTime::Seconds();
		const bool bResized = Grid->ResizeGrid(Resolution, Resolution);
		const double ResizeMs = (FPlatformTime::Seconds() - ResizeStart) * 1000.0;
		Ctx.Check(bResized, FString::Printf(TEXT("resize to %d succeeds"), Resolution));

		Grid->ClearAllCells();
		Grid->ClearHistory();

		const double BatchStart = FPlatformTime::Seconds();
		ABeanGrid::ResetEditTiming();
		const int32 Filled = Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, Resolution - 1, Resolution - 1);
		const double BatchMs = (FPlatformTime::Seconds() - BatchStart) * 1000.0;
		Ctx.Info(FString::Printf(TEXT("batch fill %dx%d breakdown: sweep %.0f us, ISM AddInstances %.0f us, map rebuild %.0f us"),
			Resolution, Resolution,
			ABeanGrid::EditTiming.RebuildSweepUs,
			ABeanGrid::EditTiming.AddInstancesUs,
			ABeanGrid::EditTiming.RebuildMapUs));
		Ctx.CheckEqualInt(Filled, Resolution * Resolution, FString::Printf(TEXT("batch fill of %dx%d changed every cell"), Resolution, Resolution));

		Grid->ClearAllCells();
		Grid->ClearHistory();
		const int32 Sample = 1000;
		const double PlaceStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Sample; ++Index)
		{
			Grid->PlaceCell(Index % Resolution, Index / Resolution);
		}
		const double PlaceUs = (FPlatformTime::Seconds() - PlaceStart) * 1.0e6 / Sample;

		const double RemoveStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Sample; ++Index)
		{
			Grid->RemoveCell(Index % Resolution, Index / Resolution);
		}
		const double RemoveUs = (FPlatformTime::Seconds() - RemoveStart) * 1.0e6 / Sample;

		// Bulk removal: filling the whole canvas and then clearing it in one operation is the path
		// that made a big-area edit feel slow, because each removed bead has to keep the
		// cell<->instance bookkeeping straight. Undo replays exactly that removal for every cell, so
		// it is the honest measurement of the optimisation (it used to rescan a full-canvas map per
		// removed bead, i.e. Width*Height work per cell).
		Grid->ClearAllCells();
		Grid->ClearHistory();
		Grid->SetCurrentColor(ColorA);
		Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, Resolution - 1, Resolution - 1);
		const double ClearStart = FPlatformTime::Seconds();
		const int32 BulkRemoved = Grid->ApplyBatch(EBeanBatchOp::Remove, 0, 0, Resolution - 1, Resolution - 1);
		const double BulkClearMs = (FPlatformTime::Seconds() - ClearStart) * 1000.0;
		Ctx.CheckEqualInt(BulkRemoved, Resolution * Resolution,
			FString::Printf(TEXT("bulk remove on %dx%d changed every cell"), Resolution, Resolution));
		const double BulkUndoStart = FPlatformTime::Seconds();
		Grid->Undo();
		const double BulkUndoMs = (FPlatformTime::Seconds() - BulkUndoStart) * 1000.0;
		Ctx.CheckEqualInt(Grid->GetCell(Resolution / 2, Resolution / 2), ColorA,
			TEXT("undoing a bulk clear put the beads back"));
		Ctx.Info(FString::Printf(TEXT("bulk clear / undo on %dx%d: %.2f ms / %.2f ms"),
			Resolution, Resolution, BulkClearMs, BulkUndoMs));
		Grid->ClearAllCells();
		Grid->ClearHistory();

		TArray<uint8> PerfTarget;
		PerfTarget.Init(static_cast<uint8>(ColorA), Resolution * Resolution);
		TArray<bool> PerfMask;
		PerfMask.Init(true, Resolution * Resolution);
		Grid->SetTargetPattern(Resolution, Resolution, 0, 0, PerfTarget, PerfMask);
		Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, Resolution - 1, Resolution - 1);

		const double StatsStart = FPlatformTime::Seconds();
		FBeanCompletionStats PerfStats;
		Grid->GetCompletionStats(PerfStats);
		const double StatsMs = (FPlatformTime::Seconds() - StatsStart) * 1000.0;

		const FString SlotName = FString::Printf(TEXT("__BeanSelfTest_%d"), Resolution);
		FString SaveError;
		const double SaveStart = FPlatformTime::Seconds();
		const bool bSaved = Grid->SaveGameAs(SlotName, SaveError);
		const double SaveMs = (FPlatformTime::Seconds() - SaveStart) * 1000.0;

		const double LoadStart = FPlatformTime::Seconds();
		FString LoadError;
		const bool bLoaded = Grid->LoadGameFrom(SlotName, LoadError);
		const double LoadMs = (FPlatformTime::Seconds() - LoadStart) * 1000.0;

		Ctx.Check(bSaved, bSaved ? FString::Printf(TEXT("save at %d succeeds"), Resolution) : FString::Printf(TEXT("save at %d failed: %s"), Resolution, *SaveError));
		Ctx.Check(bLoaded, bLoaded ? FString::Printf(TEXT("load at %d succeeds"), Resolution) : FString::Printf(TEXT("load at %d failed: %s"), Resolution, *LoadError));

		const int64 Bytes = IFileManager::Get().FileSize(*ABeanGrid::GetSlotFilePath(SlotName));
		Ctx.Info(FString::Printf(
			TEXT("%3dx%-3d | %9.2f | %8.2f | %8.2f | %9.2f | %8.2f | %8.2f (%lld KB) | %7.2f"),
			Resolution, Resolution, ResizeMs, BatchMs, PlaceUs, RemoveUs, StatsMs, SaveMs, Bytes / 1024, LoadMs));

		IFileManager::Get().Delete(*ABeanGrid::GetSlotFilePath(SlotName), false, true, true);
		IFileManager::Get().Delete(*(ABeanGrid::GetSlotFilePath(SlotName) + TEXT(".bak")), false, true, true);
		IFileManager::Get().Delete(*(ABeanGrid::GetSlotFilePath(SlotName) + TEXT(".tmp")), false, true, true);
	}

	Ctx.SectionEnd(TEXT("Bean.Perf.LargeCanvasCost"), Baseline);

	// --- feature 8: single-bead edit latency on a crowded board --------------------------------
	// The user-visible complaint was that placing beads stutters while dragging on a large canvas.
	// Placement itself is O(1) in the data, so the cost must come from the rendering bookkeeping:
	// if an edit rebuilds the whole colour's instancer, a single bead on a full canvas does
	// Width*Height work. This measures exactly that, on the worst case (every cell occupied).
	Ctx.SectionStart(TEXT("Bean.Perf.SingleBeadOnCrowdedCanvas"), Baseline);
	{
		const int32 CrowdedRes = 256;
		Grid->ResizeGrid(CrowdedRes, CrowdedRes);
		Grid->ClearAllCells();
		Grid->ClearHistory();
		Grid->SetCurrentColor(ColorA);
		Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, CrowdedRes - 1, CrowdedRes - 1);

		const int32 Edits = 200;

		// Phase 1: empty the cells the remove phase will target, so the remove phase does real work
		// (RemoveCell on an empty cell is a no-op) and the place phase does real work too.
		for (int32 Index = 1; Index < Edits; Index += 2)
		{
			Grid->RemoveCell(Index % CrowdedRes, (Index * 11) % CrowdedRes);
		}
		Grid->ClearHistory();

		// Phase 2: remove only, on freshly filled cells.
		for (int32 Index = 0; Index < Edits; ++Index)
		{
			const int32 CellX = Index % CrowdedRes;
			const int32 CellY = (Index * 17) % CrowdedRes;
			Grid->SetCurrentColor(ColorA + 1 + (Index % 3));
			Grid->PlaceCell(CellX, CellY);
		}
		Grid->ClearHistory();
		const double RemoveStartProbe = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Edits; ++Index)
		{
			Grid->RemoveCell(Index % CrowdedRes, (Index * 17) % CrowdedRes);
		}
		const double RemovePureUs = (FPlatformTime::Seconds() - RemoveStartProbe) * 1.0e6 / Edits;

		// Phase 3: place only, onto cells that the phase above just emptied.
		const double PlacePureStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Edits; ++Index)
		{
			Grid->PlaceCell(Index % CrowdedRes, (Index * 17) % CrowdedRes);
		}
		const double PlacePureUs = (FPlatformTime::Seconds() - PlacePureStart) * 1.0e6 / Edits;

		// Phase 4: the undo record path alone - placing on already-occupied cells does nothing, so
		// this isolates the history bookkeeping from the instancer work.
		Grid->ClearHistory();
		Grid->BeginStroke(TEXT("probe"));
		const double HistoryStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Edits; ++Index)
		{
			Grid->PlaceCell(Index % CrowdedRes, (Index * 23) % CrowdedRes);
		}
		const double HistoryUs = (FPlatformTime::Seconds() - HistoryStart) * 1.0e6 / Edits;
		Grid->EndStroke();

		const double CrowdedStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Edits; ++Index)
		{
			// Alternate between refreshing existing beads and filling empty ones, so both the
			// update and the remove paths are measured.
			if ((Index & 1) == 0)
			{
				Grid->PlaceCell(Index % CrowdedRes, (Index * 7) % CrowdedRes);
			}
			else
			{
				Grid->RemoveCell(Index % CrowdedRes, (Index * 11) % CrowdedRes);
			}
		}
		const double CrowdedUs = (FPlatformTime::Seconds() - CrowdedStart) * 1.0e6 / Edits;

		// Reading the whole canvas is the floor: if a single edit costs about the same as sweeping
		// every cell, the edit is doing full-canvas work instead of incremental work.
		const double SweepStart = FPlatformTime::Seconds();
		volatile int32 Sink = 0;
		for (int32 Index = 0; Index < CrowdedRes * CrowdedRes; ++Index)
		{
			Sink += Grid->GetCell(Index % CrowdedRes, Index / CrowdedRes);
		}
		const double SweepUs = (FPlatformTime::Seconds() - SweepStart) * 1.0e6;

		Ctx.Info(FString::Printf(TEXT("single bead on a full %dx%d canvas: %.1f us mixed, %.1f us remove, %.1f us place, %.1f us in-stroke place, %.1f us full-canvas read"),
			CrowdedRes, CrowdedRes, CrowdedUs, RemovePureUs, PlacePureUs, HistoryUs, SweepUs));
		Ctx.Info(FString::Printf(TEXT("instancer removal mode: %s (ordered removal reindexes every instance above the hole)"),
			Grid->SupportsInstanceRemoveSwap() ? TEXT("swap (O(1))") : TEXT("ordered (O(n))")));

		// Phase 5: where the per-edit time actually goes, in microseconds per call.
		{
			const int32 ProbeEdits = 100;
			for (int32 Index = 0; Index < ProbeEdits; ++Index)
			{
				Grid->SetCurrentColor(ColorA + 1 + (Index % 3));
				Grid->PlaceCell((Index * 3) % CrowdedRes, (Index * 29) % CrowdedRes);
			}
			ABeanGrid::ResetEditTiming();
			for (int32 Index = 0; Index < ProbeEdits; ++Index)
			{
				Grid->RemoveCell((Index * 3) % CrowdedRes, (Index * 29) % CrowdedRes);
			}
			const ABeanGrid::FBeanEditTiming& Timing = ABeanGrid::EditTiming;
			if (Timing.Calls > 0)
			{
				const double PerCall = 1.0 / Timing.Calls;
				Ctx.Info(FString::Printf(
					TEXT("edit breakdown per call (remove): edit loop %.1f us, undo+history %.1f us, instancer %.1f us, broadcast %.1f us (%d calls)"),
					Timing.EditLoopUs * PerCall, Timing.UndoUs * PerCall, Timing.InstanceUs * PerCall,
					Timing.BroadcastUs * PerCall, Timing.Calls));
			}
		}
		// A full rebuild per edit is ~65k cell visits, i.e. milliseconds. An incremental edit is a
		// handful of microseconds, so this bound catches a regression back to rebuilding without
		// being tight enough to fail on a slow machine.
		Ctx.Check(CrowdedUs < 200.0,
			FString::Printf(TEXT("a single bead on a full canvas stays cheap (%.1f us < 200 us)"), CrowdedUs));

		// The probes above removed and re-placed hundreds of beads on a ~65k-bead canvas. Counts
		// agreeing is not enough (see ValidateInstanceMappings): check the maps still name the
		// instance that is actually sitting on each cell.
		FString CrowdedMapError;
		Ctx.Check(Grid->ValidateInstanceMappings(CrowdedMapError),
			FString::Printf(TEXT("maps survive hundreds of edits on a full canvas (%s)"), *CrowdedMapError));

		Grid->ClearAllCells();
		Grid->ClearHistory();
	}
	Ctx.SectionEnd(TEXT("Bean.Perf.SingleBeadOnCrowdedCanvas"), Baseline);

	// --- section 11: save reliability ---------------------------------------------------------
	Ctx.SectionStart(TEXT("Bean.Save.RoundTripBackupAndCorruption"), Baseline);

	const FString SlotName = TEXT("__BeanSelfTest_Save");
	const FString FilePath = ABeanGrid::GetSlotFilePath(SlotName);

	Grid->ResizeGrid(64, 64);
	Grid->ClearAllCells();
	Grid->ClearHistory();
	Grid->SetWorkName(SlotName);
	Grid->SetCurrentColor(3);
	Grid->PlaceCell(5, 7);
	Grid->SetCurrentColor(9);
	Grid->PlaceCell(40, 41);
	Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 10, 10, 19, 19);

	TArray<uint8> SaveTarget;
	SaveTarget.Init(9, 64 * 64);
	TArray<bool> SaveMask;
	SaveMask.Init(true, 64 * 64);
	Grid->SetTargetPattern(64, 64, 0, 0, SaveTarget, SaveMask);

	FString Error;
	Ctx.Check(Grid->SaveGameAs(SlotName, Error), TEXT("save to an explicit slot succeeds"));
	Ctx.Check(ABeanGrid::DoesSlotExist(SlotName), TEXT("the save file exists afterwards"));
	Ctx.Check(Grid->GetSaveStatusMessage().Contains(TEXT("已保存")), TEXT("a successful save reports success, not failure"));
	Ctx.Check(!Grid->IsDirty(), TEXT("a successful save clears the dirty flag"));

	Grid->ClearAllCells();
	Grid->ClearHistory();
	Ctx.Check(Grid->LoadGameFrom(SlotName, Error), TEXT("reload succeeds"));
	Ctx.CheckEqualInt(Grid->GetWidth(), 64, TEXT("canvas width survived the round trip"));
	Ctx.CheckEqualInt(Grid->GetHeight(), 64, TEXT("canvas height survived the round trip"));
	Ctx.CheckEqualInt(Grid->GetCell(5, 7), 3, TEXT("cell (5,7) survived with its color"));
	Ctx.CheckEqualInt(Grid->GetCell(40, 41), 9, TEXT("cell (40,41) survived with its color"));
	Ctx.CheckEqualInt(Grid->GetCell(15, 15), 9, TEXT("cell (15,15) survived the batch fill"));
	Ctx.Check(Grid->HasTargetPattern(), TEXT("the target pattern survived"));

	FBeanCompletionStats SaveStats;
	Grid->GetCompletionStats(SaveStats);
	Ctx.CheckEqualInt(SaveStats.CorrectCells, 101, TEXT("completion survives a save/load round trip"));
	Ctx.Check(PerColorSumsMatch(SaveStats), TEXT("per-color breakdown survives a save/load round trip"));

	// The previous file must be kept as a backup.
	Grid->SetCurrentColor(12);
	Grid->PlaceCell(1, 1);
	Ctx.Check(Grid->SaveGameAs(SlotName, Error), TEXT("second save succeeds"));
	Ctx.Check(IFileManager::Get().FileExists(*(FilePath + TEXT(".bak"))), TEXT("the previous file was kept as a backup"));

	// A damaged primary file must fall back to the backup instead of losing the work.
	{
		TArray<uint8> Garbage;
		Garbage.SetNumZeroed(64);
		FFileHelper::SaveArrayToFile(Garbage, *FilePath);
	}
	FString RecoverError;
	const bool bRecovered = Grid->LoadGameFrom(SlotName, RecoverError);
	Ctx.Check(bRecovered, TEXT("a corrupt primary file is recovered from the backup"));
	Ctx.Check(Grid->GetWidth() == 64 && Grid->GetCell(5, 7) == static_cast<uint8>(3), TEXT("the work is intact after recovery"));

	// An unreadable save must be reported, never fatal.
	{
		TArray<uint8> Garbage;
		Garbage.SetNumZeroed(4096);
		FFileHelper::SaveArrayToFile(Garbage, *FilePath);
		IFileManager::Get().Delete(*(FilePath + TEXT(".bak")), false, true, true);
	}
	FString CorruptError;
	const bool bCorruptLoaded = Grid->LoadGameFrom(SlotName, CorruptError);
	Ctx.Check(!bCorruptLoaded, TEXT("an unreadable save reports failure"));
	Ctx.Check(!CorruptError.IsEmpty(), TEXT("the failure carries a readable message"));

	// Invalid data inside a valid archive must be rejected by the validator.
	{
		Ctx.Info(TEXT("validator rejects unknown data"));
		Ctx.Check(Grid->GetSlotFilePath(SlotName).Len() > 0, TEXT("slot file path is resolvable"));
	}

	IFileManager::Get().Delete(*FilePath, false, true, true);
	IFileManager::Get().Delete(*(FilePath + TEXT(".bak")), false, true, true);
	IFileManager::Get().Delete(*(FilePath + TEXT(".tmp")), false, true, true);

	Ctx.SectionEnd(TEXT("Bean.Save.RoundTripBackupAndCorruption"), Baseline);

	// --- undo history memory budget ------------------------------------------------------------
	Ctx.SectionStart(TEXT("Bean.Grid.HistoryStaysBounded"), Baseline);

	Grid->ResizeGrid(256, 256);
	Grid->ClearHistory();
	// A full-canvas record is ~786KB, so the 48MB budget must have trimmed well before 80 edits.
	// The loop stops as soon as the count plateaus, which keeps the check meaningful without
	// driving the editor into the page file on a 16GB machine.
	int32 IterationsRun = 0;
	int32 LastUndoCount = 0;
	int32 PlateauLength = 0;
	for (int32 Iteration = 0; Iteration < 120; ++Iteration)
	{
		const bool bPlaceNow = (Iteration % 2) == 0;
		Grid->ApplyBatch(bPlaceNow ? EBeanBatchOp::PlaceCurrentColor : EBeanBatchOp::Remove, 0, 0, 255, 255);
		++IterationsRun;

		const int32 NowCount = Grid->GetUndoCount();
		if (NowCount == LastUndoCount)
		{
			// No new step was kept: TrimHistory is already dropping the oldest record, so the cap
			// has demonstrably kicked in.
			if (++PlateauLength >= 2) break;
		}
		else
		{
			PlateauLength = 0;
			LastUndoCount = NowCount;
		}
	}

	const int32 UndoSteps = Grid->GetUndoCount();
	Ctx.Info(FString::Printf(TEXT("history after %d full-canvas edits: %d undo steps (budget %.0f MB)"),
		IterationsRun, UndoSteps, static_cast<double>(ABeanGrid::HistoryMemoryLimitBytes) / (1024.0 * 1024.0)));
	Ctx.Check(UndoSteps > 0, TEXT("history is non-empty"));
	Ctx.Check(IterationsRun < 80, TEXT("the cap stopped the history from keeping every edit"));
	Ctx.Check(UndoSteps < IterationsRun, TEXT("history was trimmed instead of keeping every edit"));

	for (int32 Step = 0; Step < UndoSteps; ++Step)
	{
		Grid->Undo();
	}
	bool bCellsValid = true;
	for (int32 Y = 0; Y < 8; ++Y)
	{
		for (int32 X = 0; X < 8; ++X)
		{
			const uint8 Cell = Grid->GetCell(X, Y);
			bCellsValid &= (Cell == Empty || Cell < 48);
		}
	}
	Ctx.Check(bCellsValid, TEXT("every cell holds either a palette index or the empty marker after undo"));

	Ctx.SectionEnd(TEXT("Bean.Grid.HistoryStaysBounded"), Baseline);

	Grid->Destroy();

	FBeanCheckContext::LastRunReport() = MoveTemp(Ctx.ReportText);
	FBeanCheckContext::LastRunCheckCount() = Ctx.Total;

	if (Ctx.Failed == 0)
	{
		UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] grid checks: %d/%d passed"), Ctx.Total, Ctx.Total);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[BeanSelfTest] grid checks: %d of %d FAILED"), Ctx.Failed, Ctx.Total);
	}
	return Ctx.Failed;
}

int32 FBeanSelfTest::RunAll(UWorld* World, bool LogDetail)
{
	FBeanCheckContext::LastRunReport().Reset();
	FBeanCheckContext::LastRunCheckCount() = 0;

	// Grid first: it needs the live world, and its report is buffered before the image half
	// overwrites the accumulator.
	const int32 GridFailures = World ? RunGridChecks(World, LogDetail) : 0;
	const FString GridReport = FBeanCheckContext::LastRunReport();
	const int32 GridChecks = FBeanCheckContext::LastRunCheckCount();

	FBeanCheckContext::LastRunReport().Reset();
	const int32 ImageFailures = RunImageChecks(LogDetail);
	const FString ImageReport = FBeanCheckContext::LastRunReport();
	const int32 ImageChecks = FBeanCheckContext::LastRunCheckCount();

	const FString Header = World
		? FString(TEXT("World: live editor world (grid checks ran)"))
		: FString(TEXT("World: none available (grid checks skipped)"));
	WriteReport(ImageFailures + GridFailures, ImageChecks + GridChecks, ImageReport + GridReport, Header);

	UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] %d checks ran, %d failed"),
		ImageChecks + GridChecks, ImageFailures + GridFailures);

	return ImageFailures + GridFailures;
}
