#pragma once

#include "CoreMinimal.h"
#include "BeanPalette.h"

struct FImage;

/**
 * Import pipeline: source picture -> canvas-sized pattern of palette indices.
 *
 * This is the one place that turns an image file into beads, so the interactive import dialog,
 * the preview panel and any future batch import all produce identical results from identical
 * settings (a hard acceptance criterion: "same picture + same settings = same output").
 *
 * It deliberately sits on top of FBeanPalette's existing CIE L*a*b* machinery instead of
 * replacing it:
 *   - the block quantizer (K-Means largest cluster + chroma-aware neutral suppression +
 *     despeckle) does the actual downscale-and-match, see FBeanPalette::QuantizeFeature;
 *   - colour counting and the "how many colours does this need" question use
 *     FBeanPalette::SelectPaletteSubset;
 *   - transparency rules live in FBeanPalette (A < 128 = no bead, a block needs one solid pixel).
 *
 * The class owns no UObject, no texture and no render resource: the output is plain data, so it is
 * safe to call from a worker thread once the UI needs the conversion to stop blocking the game
 * thread. Anything that creates a UObject/UTexture must stay on the game thread.
 */
struct BEAN_API FBeanConvertOptions
{
	/** Palette index used for "no bead here" (normally ABeanGrid::EmptyCell). */
	uint8 EmptyValue = 255;

	/** Crop rectangle in SOURCE image pixels, inclusive. Defaults to the whole picture. */
	int32 CropMinX = 0;
	int32 CropMinY = 0;
	int32 CropMaxX = 0;
	int32 CropMaxY = 0;
	bool bHasCrop = false;

	/** Canvas size in beads; the pattern always fills exactly this box. */
	int32 CanvasWidth = 64;
	int32 CanvasHeight = 64;

	/**
	 * How many of the palette's colours the picture may use. 0 (or >= palette size) = all of them.
	 * The subset is chosen from the picture itself, so "use 16 colours" really means "the 16 that
	 * describe THIS picture", not palette entries 0..15.
	 */
	int32 MaxColors = 0;

	/**
	 * Ordered (Floyd-Steinberg) error diffusion. Off by default: at one bead per source pixel a
	 * 48-colour palette already dithers into visible speckle that costs clarity, and the block
	 * quantizer is the better answer for photographs. Enable it only for deliberately flat,
	 * poster-like art where banding is the bigger problem. It also switches the canvas-size
	 * reduction from block averaging to a resample, because averaging first would cancel the
	 * dithering it is meant to add.
	 */
	bool bDither = false;
};

/** Result of one conversion. Plain data only; safe to build off the game thread. */
struct BEAN_API FBeanConvertResult
{
	/** CanvasWidth * CanvasHeight palette indices into the caller's palette, EmptyValue = no bead. */
	TArray<uint8> Cells;
	int32 Width = 0;
	int32 Height = 0;

	/** Distinct palette indices actually used. */
	int32 ColorCount = 0;

	/** Cells that hold a bead (i.e. Cells[i] != EmptyValue). */
	int32 PlacedCells = 0;

	/** True when the picture had transparent pixels, so the pattern has holes by design. */
	bool bHasTransparency = false;

	/**
	 * Palette indices used, ascending. This is the "actual colours used" readout the preview
	 * panel shows, and the subset the player can turn into a shopping list later.
	 */
	TArray<int32> UsedColors;

	/** Canvas-sized BGRA8 preview of the pattern on a light board, for the preview panel. */
	FImage Preview;

	FString Error;
};

class BEAN_API FBeanImageConverter
{
public:
	/**
	 * Quantize Source onto a CanvasWidth x CanvasHeight pattern.
	 *
	 * Source must be BGRA8 sRGB (what FImageUtils::LoadImage returns: PNG/JPEG/BMP through the
	 * engine's image wrapper, with transparency intact). Returns false and fills OutResult.Error
	 * with a player-readable reason on unusable input — callers must show that instead of crashing.
	 */
	static bool Convert(const FImage& Source, const TArray<FBeanPaletteColor>& Palette,
		const FBeanConvertOptions& Options, FBeanConvertResult& OutResult);

	/** Load a picture from disk, reporting a readable error instead of failing silently. */
	static bool LoadSourceImage(const FString& FilePath, FImage& OutImage, FString& OutError);

	/** Fill Options.Crop* with the whole picture, the default before the player crops anything. */
	static void SetFullCrop(FBeanConvertOptions& Options, const FImage& Source);

	/**
	 * Suggested crop that keeps the subject, as a square/landscape/portrait box matching the
	 * canvas aspect. Returns false when the subject cannot be told apart (photo whose content
	 * reaches every corner), in which case the caller must start from the full picture and let the
	 * player crop by hand — never guess.
	 */
	static bool SuggestCrop(const FImage& Source, int32 CanvasWidth, int32 CanvasHeight,
		int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY);

	/**
	 * Cumulative micro-second breakdown of the conversion stages, so the self-test report can show
	 * where a big picture spends its time instead of only the total. Reset by the self-test.
	 */
	struct FBeanConvertTiming
	{
		double FitUs = 0.0;      // source -> canvas-aspect working image (resample)
		double PaletteUs = 0.0;  // Lab build + picking the picture's own colour subset
		double QuantizeUs = 0.0; // per-bead colour decision (block quantizer or dither)
		double StatsUs = 0.0;    // statistics + preview
		double TotalUs = 0.0;
		int32 Calls = 0;
	};
	static FBeanConvertTiming ConvertTiming;
	static void ResetConvertTiming();
};
