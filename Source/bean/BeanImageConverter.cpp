// Bean image import pipeline. See BeanImageConverter.h for the contract.

#include "BeanImageConverter.h"

#include "Async/ParallelFor.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"

FBeanImageConverter::FBeanConvertTiming FBeanImageConverter::ConvertTiming;

void FBeanImageConverter::ResetConvertTiming()
{
	ConvertTiming = FBeanConvertTiming();
}

namespace
{
	/** Put a player-readable reason in front of whatever the engine reported. */
	void PrependError(FString& OutError, const TCHAR* What, const FString& Detail)
	{
		OutError = Detail.IsEmpty() ? FString(What) : FString::Printf(TEXT("%s：%s"), What, *Detail);
	}

	/** Nearest palette entry for an sRGB triplet, measured in CIE L*a*b*. */
	int32 NearestPaletteIndexFor(const TArray<FVector3f>& LabPalette, float R, float G, float B)
	{
		const FColor Px(
			(uint8)FMath::Clamp<int32>(FMath::RoundToInt(R), 0, 255),
			(uint8)FMath::Clamp<int32>(FMath::RoundToInt(G), 0, 255),
			(uint8)FMath::Clamp<int32>(FMath::RoundToInt(B), 0, 255),
			255);
		return FBeanPalette::NearestColorIndexLab(LabPalette, Px);
	}

	/**
	 * sRGB byte <-> linear 16-bit lookup tables, built once.
	 *
	 * The area average below has to happen in linear light or a big photo darkens the wrong way, and
	 * the engine's resampler pays for that with a pow() per source pixel (119 ms for one 6000x6000
	 * photo, measured). A table turns it into an array index, which is what makes the loop below
	 * cheap enough to be worth parallelising.
	 */
	struct FSRGBTables
	{
		uint16 ToLinear[256];
		uint8 FromLinear[65536];

		FSRGBTables()
		{
			for (int32 i = 0; i < 256; ++i)
			{
				const uint8 Byte = (uint8)i;
				const float Linear = FLinearColor::FromSRGBColor(FColor(Byte, Byte, Byte, 255)).R;
				ToLinear[i] = (uint16)FMath::Clamp<int32>(FMath::RoundToInt(Linear * 65535.0f), 0, 65535);
			}
			for (int32 i = 0; i < 65536; ++i)
			{
				const float Linear = (float)i / 65535.0f;
				FromLinear[i] = FLinearColor(Linear, Linear, Linear, 1.0f).ToFColor(/*bSRGB=*/true).R;
			}
		}
	};

	const FSRGBTables& GetSRGBTables()
	{
		static const FSRGBTables Tables;
		return Tables;
	}

	/** Bilinear tap in linear light, alpha-weighted -- the magnifying case. */
	FColor SampleBilinearLinear(const FSRGBTables& Tables, const FColor* Src, int32 SrcW, int32 SrcH, int32 Dx, int32 Dy, int32 DstW, int32 DstH)
	{
		const float Sx = ((float)Dx + 0.5f) * (float)SrcW / (float)DstW - 0.5f;
		const float Sy = ((float)Dy + 0.5f) * (float)SrcH / (float)DstH - 0.5f;
		const int32 X0 = FMath::Clamp(FMath::FloorToInt(Sx), 0, SrcW - 1);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Sy), 0, SrcH - 1);
		const int32 X1 = FMath::Min(X0 + 1, SrcW - 1);
		const int32 Y1 = FMath::Min(Y0 + 1, SrcH - 1);
		const float Fx = FMath::Clamp(Sx - (float)X0, 0.0f, 1.0f);
		const float Fy = FMath::Clamp(Sy - (float)Y0, 0.0f, 1.0f);

		const FColor* Corners[4] =
		{
			Src + (int64)Y0 * SrcW + X0,
			Src + (int64)Y0 * SrcW + X1,
			Src + (int64)Y1 * SrcW + X0,
			Src + (int64)Y1 * SrcW + X1
		};
		const float Weights[4] =
		{
			(1.0f - Fx) * (1.0f - Fy),
			Fx * (1.0f - Fy),
			(1.0f - Fx) * Fy,
			Fx * Fy
		};

		double SumR = 0.0, SumG = 0.0, SumB = 0.0, SumA = 0.0, Weight = 0.0;
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const FColor& Px = *Corners[Corner];
			const double WeightedAlpha = (double)Px.A * Weights[Corner];
			SumA += WeightedAlpha;
			Weight += Weights[Corner];
			if (Px.A > 0)
			{
				SumR += (double)Tables.ToLinear[Px.R] * WeightedAlpha;
				SumG += (double)Tables.ToLinear[Px.G] * WeightedAlpha;
				SumB += (double)Tables.ToLinear[Px.B] * WeightedAlpha;
			}
		}

		FColor Out(0, 0, 0, 0);
		if (SumA > 0.0)
		{
			Out.R = Tables.FromLinear[(int32)FMath::Clamp(SumR / SumA, 0.0, 65535.0)];
			Out.G = Tables.FromLinear[(int32)FMath::Clamp(SumG / SumA, 0.0, 65535.0)];
			Out.B = Tables.FromLinear[(int32)FMath::Clamp(SumB / SumA, 0.0, 65535.0)];
		}
		if (Weight > 0.0)
		{
			Out.A = (uint8)FMath::Clamp(FMath::RoundToDouble(SumA / Weight), 0.0, 255.0);
		}
		return Out;
	}

	/** Average each destination pixel's source rectangle in linear light, alpha-weighted. */
	void ResampleImageAreaAverage(const FColor* Src, int32 SrcW, int32 SrcH, FColor* Dst, int32 DstW, int32 DstH)
	{
		const FSRGBTables& Tables = GetSRGBTables();
		ParallelFor(DstH, [&](int32 Dy)
		{
			const int64 Y0 = (int64)Dy * SrcH / DstH;
			const int64 Y1 = FMath::Max<int64>(Y0 + 1, (int64)(Dy + 1) * SrcH / DstH);
			for (int32 Dx = 0; Dx < DstW; ++Dx)
			{
				const int64 X0 = (int64)Dx * SrcW / DstW;
				const int64 X1 = FMath::Max<int64>(X0 + 1, (int64)(Dx + 1) * SrcW / DstW);

				FColor Out(0, 0, 0, 0);
				if (X1 - X0 == 1 && Y1 - Y0 == 1)
				{
					// Magnifying: each destination pixel lands inside a single source pixel, where an
					// area average degenerates to nearest neighbour. A small picture stretched onto a
					// big canvas would show hard blocks before quantization even sees it.
					Dst[(int64)Dy * DstW + Dx] = SampleBilinearLinear(Tables, Src, SrcW, SrcH, Dx, Dy, DstW, DstH);
					continue;
				}

				uint64 SumR = 0, SumG = 0, SumB = 0, SumA = 0, Count = 0;
				for (int64 Sy = Y0; Sy < Y1; ++Sy)
				{
					const FColor* Row = Src + Sy * (int64)SrcW;
					for (int64 Sx = X0; Sx < X1; ++Sx)
					{
						const FColor& Px = Row[Sx];
						const uint64 A = Px.A;
						SumA += A;
						++Count;
						if (A == 0)
						{
							continue;
						}
						// Weighted by alpha (premultiplied): without it, the semi-transparent rim of
						// a cut-out would drag the neighbouring colour towards black.
						SumR += (uint64)Tables.ToLinear[Px.R] * A;
						SumG += (uint64)Tables.ToLinear[Px.G] * A;
						SumB += (uint64)Tables.ToLinear[Px.B] * A;
					}
				}

				if (SumA > 0)
				{
					Out.R = Tables.FromLinear[(int32)FMath::Min<uint64>(SumR / SumA, 65535)];
					Out.G = Tables.FromLinear[(int32)FMath::Min<uint64>(SumG / SumA, 65535)];
					Out.B = Tables.FromLinear[(int32)FMath::Min<uint64>(SumB / SumA, 65535)];
				}
				Out.A = Count > 0 ? (uint8)FMath::Min<uint64>(SumA / Count, 255) : 0;
				Dst[(int64)Dy * DstW + Dx] = Out;
			}
		});
	}

	/**
	 * Scale-and-centre the WHOLE picture into a canvas-aspect working image, leaving the leftover
	 * band fully transparent.
	 *
	 * Import with no crop box means "use the entire picture", and a picture whose aspect differs
	 * from the canvas cannot fill it without either cropping or distorting it. Fitting keeps the
	 * whole thing visible: the source keeps its proportions and the band that is left over stays
	 * transparent, which the converter's existing alpha handling turns into empty cells.
	 */
	bool ComposeFittedImage(const FImage& Source, int32 FitW, int32 FitH, FImage& OutImage, FString& OutError)
	{
		OutError.Reset();
		if (FitW <= 0 || FitH <= 0 || Source.SizeX <= 0 || Source.SizeY <= 0)
		{
			OutError = TEXT("画布或图片尺寸无效。");
			return false;
		}

		const double Scale = FMath::Min((double)FitW / (double)Source.SizeX, (double)FitH / (double)Source.SizeY);
		const int32 ScaledW = FMath::Clamp((int32)FMath::RoundToDouble(Source.SizeX * Scale), 1, FitW);
		const int32 ScaledH = FMath::Clamp((int32)FMath::RoundToDouble(Source.SizeY * Scale), 1, FitH);

		const FColor* SrcData = Source.AsBGRA8().GetData();
		TArray<FColor> Scaled;
		Scaled.SetNumUninitialized(ScaledW * ScaledH);
		ResampleImageAreaAverage(SrcData, Source.SizeX, Source.SizeY, Scaled.GetData(), ScaledW, ScaledH);

		OutImage.Init(FitW, FitH, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FColor* DstData = OutImage.AsBGRA8().GetData();
		FMemory::Memzero(DstData, (int64)FitW * FitH * sizeof(FColor));

		const int32 OffsetX = (FitW - ScaledW) / 2;
		const int32 OffsetY = (FitH - ScaledH) / 2;
		for (int32 Y = 0; Y < ScaledH; ++Y)
		{
			FMemory::Memcpy(DstData + (int64)(Y + OffsetY) * FitW + OffsetX, Scaled.GetData() + (int64)Y * ScaledW,
				(int64)ScaledW * sizeof(FColor));
		}
		return true;
	}
}

bool FBeanImageConverter::LoadSourceImage(const FString& FilePath, FImage& OutImage, FString& OutError)
{
	OutError.Reset();

	if (FilePath.IsEmpty())
	{
		OutError = TEXT("没有选择文件。");
		return false;
	}
	if (!FPaths::FileExists(FilePath))
	{
		OutError = FString::Printf(TEXT("图片不存在或无法访问：%s"), *FilePath);
		return false;
	}

	FImage Loaded;
	if (!FImageUtils::LoadImage(*FilePath, Loaded))
	{
		PrependError(OutError, TEXT("无法读取图片（仅支持 PNG / JPG / BMP）"), FPaths::GetCleanFilename(FilePath));
		return false;
	}
	if (Loaded.SizeX <= 0 || Loaded.SizeY <= 0)
	{
		OutError = TEXT("图片内容为空或已损坏。");
		return false;
	}

	// Hard ceiling: the import keeps several copies of the picture while converting, so an
	// extreme image (16384 px on a side = 268 MP) is refused with a readable message rather than
	// exhausting memory or stalling the machine.
	constexpr int32 MaxSourceSide = 16384;
	if (Loaded.SizeX > MaxSourceSide || Loaded.SizeY > MaxSourceSide)
	{
		OutError = FString::Printf(TEXT("图片尺寸过大（%d×%d），请先缩小到 %d 像素以内。"),
			Loaded.SizeX, Loaded.SizeY, MaxSourceSide);
		return false;
	}

	// Everything downstream (quantizer, auto-crop, dithering) works on BGRA8 sRGB byte pixels,
	// which is also what keeps the preview and the export identical to what the beads render.
	Loaded.CopyTo(OutImage, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	return true;
}

void FBeanImageConverter::SetFullCrop(FBeanConvertOptions& Options, const FImage& Source)
{
	Options.CropMinX = 0;
	Options.CropMinY = 0;
	Options.CropMaxX = FMath::Max(0, Source.SizeX - 1);
	Options.CropMaxY = FMath::Max(0, Source.SizeY - 1);
	Options.bHasCrop = true;
}

bool FBeanImageConverter::SuggestCrop(const FImage& Source, int32 CanvasWidth, int32 CanvasHeight,
	int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY)
{
	OutMinX = 0;
	OutMinY = 0;
	OutMaxX = FMath::Max(0, Source.SizeX - 1);
	OutMaxY = FMath::Max(0, Source.SizeY - 1);

	if (Source.SizeX <= 0 || Source.SizeY <= 0 || CanvasWidth <= 0 || CanvasHeight <= 0)
	{
		return false;
	}
	if (Source.Format != ERawImageFormat::BGRA8 || Source.NumSlices != 1)
	{
		return false; // unexpected layout: leave cropping to the player
	}

	const FColor* Pixels = Source.AsBGRA8().GetData();
	if (!Pixels)
	{
		return false;
	}

	int32 ContentMinX = 0, ContentMinY = 0, ContentMaxX = Source.SizeX - 1, ContentMaxY = Source.SizeY - 1;
	if (!FBeanPalette::AutoCropRect(Pixels, Source.SizeX, Source.SizeY, ContentMinX, ContentMinY, ContentMaxX, ContentMaxY))
	{
		// The subject could not be told apart from the backdrop. Say so instead of cropping.
		return false;
	}

	// Fit the target canvas aspect around the subject without ever cutting into it, then slide the
	// box so it stays inside the picture. A subject that already has the wanted shape is taken as-is.
	const double TargetAspect = (double)CanvasWidth / (double)CanvasHeight;
	const int32 ContentW = ContentMaxX - ContentMinX + 1;
	const int32 ContentH = ContentMaxY - ContentMinY + 1;
	const double ContentAspect = (double)ContentW / (double)FMath::Max(1, ContentH);

	int32 BoxW, BoxH;
	if (ContentAspect > TargetAspect)
	{
		BoxW = ContentW;
		BoxH = FMath::Max(1, (int32)FMath::RoundToDouble(ContentW / TargetAspect));
	}
	else
	{
		BoxH = ContentH;
		BoxW = FMath::Max(1, (int32)FMath::RoundToDouble(ContentH * TargetAspect));
	}
	BoxW = FMath::Clamp(BoxW, 1, Source.SizeX);
	BoxH = FMath::Clamp(BoxH, 1, Source.SizeY);

	const int32 BoxX = FMath::Clamp(ContentMinX + (ContentW - BoxW) / 2, 0, Source.SizeX - BoxW);
	const int32 BoxY = FMath::Clamp(ContentMinY + (ContentH - BoxH) / 2, 0, Source.SizeY - BoxH);

	OutMinX = BoxX;
	OutMinY = BoxY;
	OutMaxX = BoxX + BoxW - 1;
	OutMaxY = BoxY + BoxH - 1;
	return true;
}

bool FBeanImageConverter::Convert(const FImage& Source, const TArray<FBeanPaletteColor>& Palette,
	const FBeanConvertOptions& Options, FBeanConvertResult& OutResult)
{
	OutResult = FBeanConvertResult();
	++ConvertTiming.Calls;
	const double ConvertStart = FPlatformTime::Seconds();
	OutResult.Width = FMath::Max(0, Options.CanvasWidth);
	OutResult.Height = FMath::Max(0, Options.CanvasHeight);

	// ---- 1. Validate everything a corrupted or extreme input could break ---------------------
	if (Options.CanvasWidth <= 0 || Options.CanvasHeight <= 0)
	{
		OutResult.Error = TEXT("画布尺寸无效。");
		return false;
	}
	if (Options.CanvasWidth > 2048 || Options.CanvasHeight > 2048)
	{
		OutResult.Error = TEXT("画布尺寸超出上限（2048）。");
		return false;
	}
	if (Palette.Num() == 0)
	{
		OutResult.Error = TEXT("调色板为空，无法转换。");
		return false;
	}
	if (Source.Format != ERawImageFormat::BGRA8 || Source.NumSlices != 1)
	{
		OutResult.Error = TEXT("图片像素格式不受支持。");
		return false;
	}
	if (Source.SizeX <= 0 || Source.SizeY <= 0)
	{
		OutResult.Error = TEXT("图片内容为空。");
		return false;
	}

	const FColor* Pixels = Source.AsBGRA8().GetData();
	if (!Pixels)
	{
		OutResult.Error = TEXT("图片像素数据不可用。");
		return false;
	}

	// ---- 2. Resolve the crop rectangle -------------------------------------------------------
	// The crop box is always interpreted at the canvas aspect ratio (the import dialog keeps it
	// that way), and the region is mapped onto the whole canvas, so nothing is stretched: a
	// 4:3 source crop with a 4:3 canvas keeps its proportions.
	//
	// No crop box means the WHOLE picture, fitted rather than cropped: the source keeps its own
	// aspect inside a canvas-aspect working image and the leftover band stays transparent. (This
	// used to take a canvas-sized window from the picture's top-left corner, which silently threw
	// away most of a wide photo and made 不裁切 impossible to express.)
	FImage Fitted;
	const bool bFitted = !Options.bHasCrop;
	if (bFitted)
	{
		// Supersample so the down-sampling below still averages real source pixels.
		const int32 Supersample = FMath::Clamp(2048 / FMath::Max(Options.CanvasWidth, Options.CanvasHeight), 1, 4);
		const int32 FitW = Options.CanvasWidth * Supersample;
		const int32 FitH = Options.CanvasHeight * Supersample;
		if (!ComposeFittedImage(Source, FitW, FitH, Fitted, OutResult.Error))
		{
			return false;
		}
		Pixels = Fitted.AsBGRA8().GetData();
	}

	const int32 PixelStrideX = bFitted ? Fitted.SizeX : Source.SizeX;
	const int32 SourceW = bFitted ? Fitted.SizeX : Source.SizeX;
	const int32 SourceH = bFitted ? Fitted.SizeY : Source.SizeY;
	const double FitDone = FPlatformTime::Seconds();
	ConvertTiming.FitUs += (FitDone - ConvertStart) * 1.0e6;

	int32 CropMinX = Options.bHasCrop ? Options.CropMinX : 0;
	int32 CropMinY = Options.bHasCrop ? Options.CropMinY : 0;
	int32 CropW = Options.bHasCrop ? (Options.CropMaxX - Options.CropMinX + 1) : SourceW;
	int32 CropH = Options.bHasCrop ? (Options.CropMaxY - Options.CropMinY + 1) : SourceH;

	CropMinX = FMath::Clamp(CropMinX, 0, FMath::Max(0, SourceW - 1));
	CropMinY = FMath::Clamp(CropMinY, 0, FMath::Max(0, SourceH - 1));

	if (CropW <= 0 || CropH <= 0)
	{
		OutResult.Error = TEXT("裁剪区域为空，请重新拉出选择框。");
		return false;
	}
	CropW = FMath::Min(CropW, SourceW - CropMinX);
	CropH = FMath::Min(CropH, SourceH - CropMinY);
	if (CropW <= 0 || CropH <= 0)
	{
		OutResult.Error = TEXT("裁剪区域超出图片范围。");
		return false;
	}

	TArray<FVector3f> FullLab;
	FBeanPalette::BuildLabPalette(Palette, FullLab);

	// ---- 3. Restrict the palette to at most MaxColors entries, chosen FROM THIS PICTURE ------
	TArray<FVector3f> QuantLab = FullLab;
	TArray<uint8> KeepMap; // quantizer index -> caller palette index
	if (Options.MaxColors > 0 && Options.MaxColors < Palette.Num())
	{
		FBeanPalette::FBeanPaletteSubset Subset;
		FBeanPalette::SelectPaletteSubset(FullLab, Pixels + (int64)CropMinY * PixelStrideX + CropMinX,
			CropW, CropH, Options.MaxColors, Subset);
		if (Subset.Num() > 0 && Subset.Num() < Palette.Num())
		{
			QuantLab.Reset();
			QuantLab.Reserve(Subset.Num());
			KeepMap.Reserve(Subset.Num());
			for (int32 i = 0; i < Subset.Num(); ++i)
			{
				QuantLab.Add(Subset.SubLab[i]);
				KeepMap.Add(Subset.Remap[i]);
			}
		}
	}
	auto ToCallerIndex = [&KeepMap](uint8 QuantIndex) -> uint8
	{
		if (KeepMap.Num() == 0)
		{
			return QuantIndex;
		}
		return (QuantIndex < (uint8)KeepMap.Num()) ? KeepMap[QuantIndex] : QuantIndex;
	};
	const double PaletteDone = FPlatformTime::Seconds();
	ConvertTiming.PaletteUs += (PaletteDone - FitDone) * 1.0e6;

	// ---- 4a. Error-diffusion path, or 4b. the block quantizer --------------------------------
	OutResult.Cells.Init(Options.EmptyValue, Options.CanvasWidth * Options.CanvasHeight);
	bool bAnyTransparent = false;

	if (Options.bDither)
	{
		// Reduce the crop to exactly one pixel per bead first: the diffusion error has to refer to
		// the bead it will actually end up on. This path uses its own box filter rather than the
		// block quantizer, because averaging is precisely what would cancel the dithering.
		// One output pixel per bead is a hard requirement: the diffusion error has to land on the
		// bead it belongs to. The pre-pass therefore resamples the CROP onto the CANVAS grid using
		// its own box filter (the block quantizer's averaging would cancel the dithering out).
		const int32 SmallW = Options.CanvasWidth;
		const int32 SmallH = Options.CanvasHeight;
		FImage Small(SmallW, SmallH, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FColor* SmallPixels = reinterpret_cast<FColor*>(Small.RawData.GetData());
		for (int32 Y = 0; Y < SmallH; ++Y)
		{
			// Destination pixel -> fractional source box inside the crop, so any crop size is
			// scaled onto the canvas without stretching or dropping rows/columns.
			const int32 Y0 = CropMinY + (int32)((int64)Y * CropH / SmallH);
			const int32 Y1 = CropMinY + (int32)((int64)(Y + 1) * CropH / SmallH);
			for (int32 X = 0; X < SmallW; ++X)
			{
				const int32 X0 = CropMinX + (int32)((int64)X * CropW / SmallW);
				const int32 X1 = CropMinX + (int32)((int64)(X + 1) * CropW / SmallW);

				float R = 0.0f, G = 0.0f, B = 0.0f;
				int32 Count = 0;
				uint8 MaxA = 0;
				const int32 SyLast = FMath::Clamp(FMath::Max(Y0, Y1 - 1), 0, SourceH - 1);
				const int32 SxLast = FMath::Clamp(FMath::Max(X0, X1 - 1), 0, SourceW - 1);
				for (int32 Sy = FMath::Clamp(Y0, 0, SourceH - 1); Sy <= SyLast; ++Sy)
				{
					for (int32 Sx = FMath::Clamp(X0, 0, SourceW - 1); Sx <= SxLast; ++Sx)
					{
						const FColor& Px = Pixels[(int64)Sy * PixelStrideX + Sx];
						MaxA = FMath::Max(MaxA, Px.A);
						if (Px.A < 128) continue;
						R += Px.R; G += Px.G; B += Px.B; ++Count;
					}
				}
				if (Count > 0)
				{
					const float Inv = 1.0f / Count;
					R *= Inv; G *= Inv; B *= Inv;
				}
				else
				{
					R = G = B = 255.0f;
				}
				SmallPixels[(int64)Y * SmallW + X] = FColor(
					(uint8)FMath::Clamp<int32>(FMath::RoundToInt(R), 0, 255),
					(uint8)FMath::Clamp<int32>(FMath::RoundToInt(G), 0, 255),
					(uint8)FMath::Clamp<int32>(FMath::RoundToInt(B), 0, 255),
					MaxA);
			}
		}

		// Float working copy so error can be pushed both ways; serpentine rows stop the classic
		// diagonal "worm" pattern of plain Floyd-Steinberg.
		TArray<float> Work;
		Work.SetNumUninitialized(SmallW * SmallH * 3);
		for (int32 i = 0; i < SmallW * SmallH; ++i)
		{
			Work[i * 3 + 0] = SmallPixels[i].R;
			Work[i * 3 + 1] = SmallPixels[i].G;
			Work[i * 3 + 2] = SmallPixels[i].B;
		}

		for (int32 Y = 0; Y < SmallH; ++Y)
		{
			const bool bReverse = (Y & 1) != 0;
			for (int32 Step = 0; Step < SmallW; ++Step)
			{
				const int32 X = bReverse ? (SmallW - 1 - Step) : Step;
				const int32 Idx = Y * SmallW + X;

				if (SmallPixels[Idx].A < 128)
				{
					bAnyTransparent = true;
					OutResult.Cells[Idx] = Options.EmptyValue;
					continue;
				}

				const int32 QuantIndex = NearestPaletteIndexFor(QuantLab,
					Work[Idx * 3 + 0], Work[Idx * 3 + 1], Work[Idx * 3 + 2]);
				const uint8 CallerIndex = ToCallerIndex((uint8)QuantIndex);
				OutResult.Cells[Idx] = CallerIndex;

				const FColor Chosen = Palette.IsValidIndex(CallerIndex)
					? Palette[CallerIndex].Color.ToFColorSRGB()
					: FColor::White;
				const float Er = Work[Idx * 3 + 0] - Chosen.R;
				const float Eg = Work[Idx * 3 + 1] - Chosen.G;
				const float Eb = Work[Idx * 3 + 2] - Chosen.B;

				const int32 Dir = bReverse ? -1 : 1;
				auto Push = [&](int32 dx, int32 dy, float Weight)
				{
					const int32 Nx = X + dx * Dir;
					const int32 Ny = Y + dy;
					if (Nx < 0 || Nx >= SmallW || Ny < 0 || Ny >= SmallH) return;
					const int32 N = (Ny * SmallW + Nx) * 3;
					Work[N + 0] = FMath::Clamp(Work[N + 0] + Er * Weight, 0.0f, 255.0f);
					Work[N + 1] = FMath::Clamp(Work[N + 1] + Eg * Weight, 0.0f, 255.0f);
					Work[N + 2] = FMath::Clamp(Work[N + 2] + Eb * Weight, 0.0f, 255.0f);
				};
				Push(1, 0, 7.0f / 16.0f);
				Push(-1, 1, 3.0f / 16.0f);
				Push(0, 1, 5.0f / 16.0f);
				Push(1, 1, 1.0f / 16.0f);
			}
		}

		// Only the despeckle polish runs afterwards; the dither pattern itself must survive, which
		// is why the block pass is told not to recompute the colours. The buffer is the full canvas
		// at one pixel per cell, which is exactly the grid QuantizeFeature expects.
		FBeanPalette::QuantizeFeature(QuantLab, SmallPixels, SmallW, SmallH,
			SmallW, SmallH, Options.EmptyValue, OutResult.Cells, true);
	}
	else
	{
		// The block quantizer averages every source rectangle itself, so a large picture handed to
		// a small canvas is correctly low-passed (which is what removes the anti-aliased edge
		// fuzz the old one-pixel-per-cell path turned into stray beads), and a small picture handed
		// to a large canvas simply repeats.
		FBeanPalette::QuantizeFeature(QuantLab, Pixels + (int64)CropMinY * PixelStrideX + CropMinX,
			CropW, CropH, Options.CanvasWidth, Options.CanvasHeight, Options.EmptyValue, OutResult.Cells, false);

		if (KeepMap.Num() > 0)
		{
			for (uint8& Cell : OutResult.Cells)
			{
				if (Cell != Options.EmptyValue) Cell = ToCallerIndex(Cell);
			}
		}
	}

	// ---- 5. Statistics + preview -------------------------------------------------------------
	const double QuantizeDone = FPlatformTime::Seconds();
	ConvertTiming.QuantizeUs += (QuantizeDone - PaletteDone) * 1.0e6;
	bool UsedFlags[256] = {};
	for (uint8 Cell : OutResult.Cells)
	{
		if (Cell == Options.EmptyValue)
		{
			bAnyTransparent = true;
			continue;
		}
		++OutResult.PlacedCells;
		if (Cell < 255)
		{
			UsedFlags[Cell] = true;
		}
	}
	for (int32 i = 0; i < 256; ++i)
	{
		if (UsedFlags[i])
		{
			OutResult.UsedColors.Add(i);
		}
	}
	OutResult.ColorCount = OutResult.UsedColors.Num();
	OutResult.bHasTransparency = bAnyTransparent;

	OutResult.Preview.Init(Options.CanvasWidth, Options.CanvasHeight, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* PreviewPixels = reinterpret_cast<FColor*>(OutResult.Preview.RawData.GetData());
	constexpr uint8 BoardR = 236, BoardG = 233, BoardB = 226;
	for (int32 i = 0; i < Options.CanvasWidth * Options.CanvasHeight; ++i)
	{
		const uint8 Cell = OutResult.Cells[i];
		if (Cell == Options.EmptyValue)
		{
			// No bead here: show the board, faintly checkered so holes read as empty rather than
			// as a very light bead colour.
			const int32 Checker = ((i % Options.CanvasWidth) / 8 + (i / Options.CanvasWidth) / 8) % 2;
			const uint8 Light = Checker == 0 ? 0 : 10;
			PreviewPixels[i] = FColor(BoardR + Light, BoardG + Light, BoardB + Light, 255);
			continue;
		}
		const FColor Color = Palette.IsValidIndex(Cell) ? Palette[Cell].Color.ToFColorSRGB() : FColor::Magenta;
		PreviewPixels[i] = FColor(Color.R, Color.G, Color.B, 255);
	}

	if (OutResult.PlacedCells == 0)
	{
		OutResult.Error = TEXT("转换后没有任何方块（图片可能全透明）。");
		return false;
	}
	const double ConvertEnd = FPlatformTime::Seconds();
	ConvertTiming.StatsUs += (ConvertEnd - QuantizeDone) * 1.0e6;
	ConvertTiming.TotalUs += (ConvertEnd - ConvertStart) * 1.0e6;
	return true;
}
