#include "BeanAssets.h"

#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	const TCHAR* CubeMaterialPath = TEXT("/Game/Bean/M_BeanCube");
	const TCHAR* GroundMaterialPath = TEXT("/Game/Bean/M_BeanGround");
	const TCHAR* OverlayMaterialPath = TEXT("/Game/Bean/M_BeanOverlay");
	const TCHAR* GhostMaterialPath = TEXT("/Game/Bean/M_BeanGhost");

	/**
	 * Shared ground grid textures, one per canvas cell count (X in the low 16 bits of the key, Y in
	 * the high 16). File scope (not a function-local static) so ResetCache() can drop them all.
	 * Rooted while alive, so a raw pointer is safe here.
	 */
	TMap<uint32, TObjectPtr<UTexture2D>> BeanGridTextureCache;
	/** Keys of BeanGridTextureCache in least-recently-used order (front = oldest). */
	TArray<uint32> BeanGridTextureCacheOrder;
	/**
	 * How many grid textures to keep alive. The biggest is 64MB (256 cells), the smallest 1MB, and
	 * a canvas only ever needs one, so a small window is enough to make switching shapes free
	 * without letting the cache grow with every size the player tries.
	 */
	constexpr int32 BeanGridTextureCacheLimit = 2;

	/** Unroot and forget every cached grid texture. */
	void DropGridTextureCache()
	{
		for (const TPair<uint32, TObjectPtr<UTexture2D>>& Pair : BeanGridTextureCache)
		{
			if (Pair.Value)
			{
				Pair.Value->RemoveFromRoot();
			}
		}
		BeanGridTextureCache.Reset();
		BeanGridTextureCacheOrder.Reset();
	}

#if WITH_EDITOR
	enum class EBeanMaterialKind
	{
		Cube,     // unlit opaque, VectorParameter "Color"
		Ground,   // unlit opaque, TextureParameter "GridTex" * VectorParameter "BaseColor"
		Overlay,  // unlit translucent, TextureParameter "OverlayTex" * ScalarParameter "Opacity"
		Ghost,    // unlit translucent, VectorParameter "Color" * ScalarParameter "Opacity"
	};

	UMaterial* BuildMaterial(UPackage* Outer, FName AssetName, EBeanMaterialKind Kind)
	{
		const bool bTranslucent = (Kind == EBeanMaterialKind::Overlay || Kind == EBeanMaterialKind::Ghost);

		UMaterial* M = NewObject<UMaterial>(Outer, AssetName, Outer != nullptr ? (RF_Public | RF_Standalone) : RF_Transient);
		M->BlendMode = bTranslucent ? BLEND_Translucent : BLEND_Opaque;
		M->SetShadingModel(EMaterialShadingModel::MSM_Unlit);
		M->SetUsageByFlag(EMaterialUsage::MATUSAGE_InstancedStaticMeshes, true);
		M->TwoSided = bTranslucent;

		UMaterialExpression* EmissiveInput = nullptr;
		UMaterialExpression* OpacityInput = nullptr;

		switch (Kind)
		{
		case EBeanMaterialKind::Cube:
		{
			UMaterialExpressionVectorParameter* Color = NewObject<UMaterialExpressionVectorParameter>(M);
			Color->ParameterName = TEXT("Color");
			Color->DefaultValue = FLinearColor::White;
			M->GetExpressionCollection().AddExpression(Color);
			EmissiveInput = Color;
			break;
		}
		case EBeanMaterialKind::Ground:
		{
			UMaterialExpressionTextureSampleParameter2D* GridTex = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
			GridTex->ParameterName = TEXT("GridTex");
			GridTex->SamplerType = SAMPLERTYPE_Color;
			M->GetExpressionCollection().AddExpression(GridTex);

			UMaterialExpressionVectorParameter* BaseColor = NewObject<UMaterialExpressionVectorParameter>(M);
			BaseColor->ParameterName = TEXT("BaseColor");
			BaseColor->DefaultValue = FLinearColor::White;
			M->GetExpressionCollection().AddExpression(BaseColor);

			UMaterialExpressionMultiply* Multiply = NewObject<UMaterialExpressionMultiply>(M);
			Multiply->A.Expression = GridTex;
			Multiply->B.Expression = BaseColor;
			M->GetExpressionCollection().AddExpression(Multiply);
			EmissiveInput = Multiply;
			break;
		}
		case EBeanMaterialKind::Overlay:
		{
			UMaterialExpressionTextureSampleParameter2D* OverlayTex = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
			OverlayTex->ParameterName = TEXT("OverlayTex");
			OverlayTex->SamplerType = SAMPLERTYPE_Color;
			M->GetExpressionCollection().AddExpression(OverlayTex);

			UMaterialExpressionScalarParameter* Opacity = NewObject<UMaterialExpressionScalarParameter>(M);
			Opacity->ParameterName = TEXT("Opacity");
			Opacity->DefaultValue = 0.6f;
			M->GetExpressionCollection().AddExpression(Opacity);

			// Opacity = overlayTexture.Alpha * OpacityParam, so empty (transparent) cells stay invisible.
			UMaterialExpressionMultiply* OpacityMul = NewObject<UMaterialExpressionMultiply>(M);
			OpacityMul->A.Expression = OverlayTex;
			OpacityMul->A.MaskR = false;
			OpacityMul->A.MaskG = false;
			OpacityMul->A.MaskB = false;
			OpacityMul->A.MaskA = true;
			OpacityMul->B.Expression = Opacity;
			M->GetExpressionCollection().AddExpression(OpacityMul);

			EmissiveInput = OverlayTex;
			OpacityInput = OpacityMul;
			break;
		}
		case EBeanMaterialKind::Ghost:
		{
			UMaterialExpressionVectorParameter* Color = NewObject<UMaterialExpressionVectorParameter>(M);
			Color->ParameterName = TEXT("Color");
			Color->DefaultValue = FLinearColor::White;
			M->GetExpressionCollection().AddExpression(Color);

			UMaterialExpressionScalarParameter* Opacity = NewObject<UMaterialExpressionScalarParameter>(M);
			Opacity->ParameterName = TEXT("Opacity");
			Opacity->DefaultValue = 0.4f;
			M->GetExpressionCollection().AddExpression(Opacity);

			EmissiveInput = Color;
			OpacityInput = Opacity;
			break;
		}
		}

#if WITH_EDITOR
		UMaterialEditorOnlyData* EditorOnlyData = M->GetEditorOnlyData();
		if (EditorOnlyData)
		{
			EditorOnlyData->EmissiveColor.Expression = EmissiveInput;
			EditorOnlyData->EmissiveColor.OutputIndex = 0;
			if (OpacityInput != nullptr)
			{
				EditorOnlyData->Opacity.Expression = OpacityInput;
				EditorOnlyData->Opacity.OutputIndex = 0;
			}
		}
		M->PostEditChange();
#endif
		return M;
	}

	UMaterial* GetOrCreate(const TCHAR* Path, EBeanMaterialKind Kind)
	{
		if (UMaterial* Loaded = LoadObject<UMaterial>(nullptr, Path))
		{
			return Loaded;
		}
		return BuildMaterial(GetTransientPackage(), NAME_None, Kind);
	}
#endif // WITH_EDITOR
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

#if WITH_EDITOR
// Editor path: load the baked asset when present, otherwise build a transient
// material at runtime (requires editor-only UMaterial expression APIs).
UMaterial* FBeanAssets::GetCubeMaterial()
{
	return GetOrCreate(CubeMaterialPath, EBeanMaterialKind::Cube);
}

UMaterial* FBeanAssets::GetGroundMaterial()
{
	return GetOrCreate(GroundMaterialPath, EBeanMaterialKind::Ground);
}

UMaterial* FBeanAssets::GetOverlayMaterial()
{
	return GetOrCreate(OverlayMaterialPath, EBeanMaterialKind::Overlay);
}

UMaterial* FBeanAssets::GetGhostMaterial()
{
	return GetOrCreate(GhostMaterialPath, EBeanMaterialKind::Ghost);
}

void FBeanAssets::ResetCache()
{
	// No persistent material cache: each Get*() re-resolves LoadObject / rebuilds.
	// The shared grid textures are dropped here too, so the next CreateGridTexture() rebuilds them.
	DropGridTextureCache();
}
#else
// Packaged builds cannot construct materials at runtime (editor-only APIs), so
// load the baked /Game/Bean assets. Run "BeanBakeAssets" in the editor once
// before packaging so the assets exist and get cooked.
UMaterial* FBeanAssets::GetCubeMaterial()
{
	return LoadObject<UMaterial>(nullptr, TEXT("/Game/Bean/M_BeanCube"));
}

UMaterial* FBeanAssets::GetGroundMaterial()
{
	return LoadObject<UMaterial>(nullptr, TEXT("/Game/Bean/M_BeanGround"));
}

UMaterial* FBeanAssets::GetOverlayMaterial()
{
	return LoadObject<UMaterial>(nullptr, TEXT("/Game/Bean/M_BeanOverlay"));
}

UMaterial* FBeanAssets::GetGhostMaterial()
{
	return LoadObject<UMaterial>(nullptr, TEXT("/Game/Bean/M_BeanGhost"));
}

void FBeanAssets::ResetCache()
{
	DropGridTextureCache();
}
#endif // WITH_EDITOR

// ---------------------------------------------------------------------------
// Baking (editor only) — run "BeanBakeAssets" in the editor console.
// ---------------------------------------------------------------------------

#if WITH_EDITOR

namespace
{
	bool BakeOne(const TCHAR* PackagePath, const TCHAR* AssetName, EBeanMaterialKind Kind)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return false;
		UMaterial* M = BuildMaterial(Pkg, FName(AssetName), Kind);
		if (!M) return false;

		const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Pkg, M, *Filename, SaveArgs);
	}
}

bool FBeanAssets::BakeAssets()
{
	bool bOk = true;
	bOk &= BakeOne(TEXT("/Game/Bean/M_BeanCube"), TEXT("M_BeanCube"), EBeanMaterialKind::Cube);
	bOk &= BakeOne(TEXT("/Game/Bean/M_BeanGround"), TEXT("M_BeanGround"), EBeanMaterialKind::Ground);
	bOk &= BakeOne(TEXT("/Game/Bean/M_BeanOverlay"), TEXT("M_BeanOverlay"), EBeanMaterialKind::Overlay);
	bOk &= BakeOne(TEXT("/Game/Bean/M_BeanGhost"), TEXT("M_BeanGhost"), EBeanMaterialKind::Ghost);
	return bOk;
}

#endif

// ---------------------------------------------------------------------------
// Textures (runtime-created; safe for packaged builds)
// ---------------------------------------------------------------------------

UTexture2D* FBeanAssets::CreateGridTexture(int32 CellsX, int32 CellsY)
{
	// One square per cell, 16 texels each (so a 256-cell canvas is the original 4096x4096 over a
	// 64m plane = 0.25m per cell) and smaller canvases get a proportionally smaller texture instead
	// of the same picture stretched over fewer cells.
	const int32 CellsXClamped = FMath::Clamp(CellsX, 1, 1024);
	const int32 CellsYClamped = FMath::Clamp(CellsY, 1, 1024);
	constexpr int32 CellTexels = 16;
	const int32 SizeX = FMath::Clamp(CellsXClamped * CellTexels, 64, 4096);
	const int32 SizeY = FMath::Clamp(CellsYClamped * CellTexels, 64, 4096);
	const uint32 Key = ((uint32)CellsXClamped << 16) | (uint32)CellsYClamped;

	// Generated once per canvas shape: the image is immutable, and building it per grid actor used
	// to cost ~200ms plus a large transient allocation on every spawn/resize.
	if (TObjectPtr<UTexture2D>* Cached = BeanGridTextureCache.Find(Key))
	{
		// Touch it, so the LRU keeps whatever the player is actually using.
		BeanGridTextureCacheOrder.Remove(Key);
		BeanGridTextureCacheOrder.Add(Key);
		return *Cached;
	}
	// Only ever build it on the game thread: CreateTexture2DFromImage creates a UObject, and a
	// non-game-thread call would trip the object-creation guard.
	if (!IsInGameThread())
	{
		return nullptr;
	}

	FImage Image(SizeX, SizeY, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());

	// Warm off-white ground with a very subtle warm-gray grid line.
	const FColor Fill(235, 232, 222);
	const FColor Line(220, 216, 205);
	const FColor Border(196, 192, 182);

	for (int32 Y = 0; Y < SizeY; ++Y)
	{
		for (int32 X = 0; X < SizeX; ++X)
		{
			FColor C = Fill;
			if (X == 0 || Y == 0 || X == SizeX - 1 || Y == SizeY - 1)
			{
				C = Border;
			}
			else if ((X % CellTexels == 0) || (Y % CellTexels == 0))
			{
				C = Line;
			}
			Data[Y * SizeX + X] = C;
		}
	}

	UTexture2D* Texture = FImageUtils::CreateTexture2DFromImage(FImageView(Image));
	if (Texture)
	{
		Texture->Filter = TF_Nearest;
		Texture->SRGB = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->UpdateResource();

		// Rooted so the shared instance survives the garbage collector; ResetCache() unroots it.
		Texture->AddToRoot();
		BeanGridTextureCache.Add(Key, Texture);
		BeanGridTextureCacheOrder.Add(Key);
		while (BeanGridTextureCacheOrder.Num() > BeanGridTextureCacheLimit)
		{
			const uint32 Oldest = BeanGridTextureCacheOrder[0];
			BeanGridTextureCacheOrder.RemoveAt(0);
			if (TObjectPtr<UTexture2D>* Evicted = BeanGridTextureCache.Find(Oldest))
			{
				// Only the cache's own reference goes away: whoever is using it holds a reference
				// through their material instance, so an eviction can never pull a live texture.
				if (*Evicted)
				{
					(*Evicted)->RemoveFromRoot();
				}
				BeanGridTextureCache.Remove(Oldest);
			}
		}
	}
	return Texture;
}

UTexture2D* FBeanAssets::CreateCrosshairTexture()
{
	constexpr int32 Size = 32;
	FImage Image(Size, Size, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());
	for (int32 i = 0; i < Size * Size; ++i)
	{
		Data[i] = FColor(0, 0, 0, 0);
	}

	constexpr int32 Center = Size / 2;
	constexpr int32 HalfArm = 8;
	constexpr int32 Thickness = 2;
	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			const bool bVerticalArm = FMath::Abs(X - Center) < Thickness && FMath::Abs(Y - Center) <= HalfArm;
			const bool bHorizontalArm = FMath::Abs(Y - Center) < Thickness && FMath::Abs(X - Center) <= HalfArm;
			if (bVerticalArm || bHorizontalArm)
			{
				Data[Y * Size + X] = FColor(255, 255, 255, 255);
			}
		}
	}

	UTexture2D* Texture = FImageUtils::CreateTexture2DFromImage(FImageView(Image));
	if (Texture)
	{
		Texture->Filter = TF_Nearest;
		Texture->SRGB = true;
		Texture->UpdateResource();
	}
	return Texture;
}

UTexture2D* FBeanAssets::CreateCubeIconTexture(const FLinearColor& Color, FVector2D* OutFrontFaceCenter, int32 IconSize)
{
	const int32 Size = FMath::Max(16, IconSize);
	FImage Image(Size, Size, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());
	for (int32 i = 0; i < Size * Size; ++i)
	{
		Data[i] = FColor(0, 0, 0, 0);
	}

	// A standard cube: a large square FRONT face plus narrow top and right faces
	// that lean up-and-right. The front face is 25% bigger than before; the lean is
	// kept small so the enlarged silhouette still fits inside the square texture.
	const float Half = Size * 0.45f;    // front face half size (square)
	const float LeanX = Size * 0.08f;   // top/right faces lean right
	const float LeanY = Size * 0.08f;   // top/right faces lean up
	// Center the whole silhouette (right/up lean is asymmetric, so nudge the front).
	const float Cx = Size * 0.5f - LeanX * 0.5f;
	const float Cy = Size * 0.5f + LeanY * 0.5f;
	if (OutFrontFaceCenter)
	{
		// Normalized centre of the front face, so callers can anchor a label onto it.
		OutFrontFaceCenter->X = Cx / Size;
		OutFrontFaceCenter->Y = Cy / Size;
	}

	// Front face corners (a square).
	const FVector2D F_TL(Cx - Half, Cy - Half);
	const FVector2D F_TR(Cx + Half, Cy - Half);
	const FVector2D F_BR(Cx + Half, Cy + Half);
	const FVector2D F_BL(Cx - Half, Cy + Half);
	// Far edges of the leaned top and right faces.
	const FVector2D T_BL = F_TL + FVector2D(LeanX, -LeanY);
	const FVector2D T_BR = F_TR + FVector2D(LeanX, -LeanY);
	const FVector2D R_BR = F_BR + FVector2D(LeanX, -LeanY);

	// Shading: brightest top, mid right, base front.
	const FLinearColor TopLinear = FLinearColor(Color.R * 1.30f, Color.G * 1.30f, Color.B * 1.30f, 1.f).GetClamped();
	const FLinearColor RightLinear = FLinearColor(Color.R * 0.68f, Color.G * 0.68f, Color.B * 0.68f, 1.f).GetClamped();
	const FColor Front = Color.ToFColor(true);
	const FColor Top = TopLinear.ToFColor(true);
	const FColor Right = RightLinear.ToFColor(true);
	const FColor Outline(12, 12, 12, 255);

	auto PointInQuad = [](int32 X, int32 Y, const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D)
	{
		const FVector2D P(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f);
		auto Sign = [&P](const FVector2D& P1, const FVector2D& P2)
		{
			return (P.X - P2.X) * (P1.Y - P2.Y) - (P1.X - P2.X) * (P.Y - P2.Y);
		};
		const float S1 = Sign(A, B), S2 = Sign(B, C), S3 = Sign(C, D), S4 = Sign(D, A);
		const bool bHasNeg = (S1 < 0) || (S2 < 0) || (S3 < 0) || (S4 < 0);
		const bool bHasPos = (S1 > 0) || (S2 > 0) || (S3 > 0) || (S4 > 0);
		return !(bHasNeg && bHasPos);
	};

	const int32 MinX = FMath::Max(0, FMath::FloorToInt(F_BL.X));
	const int32 MaxX = FMath::Min(Size - 1, FMath::CeilToInt(F_BR.X + LeanX));
	const int32 MinY = FMath::Max(0, FMath::FloorToInt(F_TL.Y - LeanY));
	const int32 MaxY = FMath::Min(Size - 1, FMath::CeilToInt(F_BR.Y));

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			FColor C(0, 0, 0, 0);
			if (PointInQuad(X, Y, F_TL, F_TR, T_BR, T_BL)) C = Top;
			if (PointInQuad(X, Y, F_TR, T_BR, R_BR, F_BR)) C = Right;
			if (X >= F_TL.X && X <= F_TR.X && Y >= F_TL.Y && Y <= F_BR.Y) C = Front;
			Data[Y * Size + X] = C;
		}
	}

	// Crisp outer outline: any opaque pixel adjacent to a transparent one. Neighbour reads
	// are bounds-checked: the cube can touch the texture border, and reading one texel
	// outside the image buffer crashed packaged builds (access violation).
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			FColor& C = Data[Y * Size + X];
			if (C.A == 0) continue;
			const bool bEdge =
				(X > 0 && Data[Y * Size + (X - 1)].A == 0) ||
				(X < Size - 1 && Data[Y * Size + (X + 1)].A == 0) ||
				(Y > 0 && Data[(Y - 1) * Size + X].A == 0) ||
				(Y < Size - 1 && Data[(Y + 1) * Size + X].A == 0);
			if (bEdge) C = Outline;
		}
	}

	UTexture2D* Texture = FImageUtils::CreateTexture2DFromImage(FImageView(Image));
	if (Texture)
	{
		Texture->Filter = TF_Nearest;
		Texture->SRGB = true;
		Texture->UpdateResource();
	}
	return Texture;
}

UTexture2D* FBeanAssets::CreateOverlayTexture(const TArray<FLinearColor>& CellColors, const TArray<bool>& HasCell,
	const TArray<int32>& CellLabels, int32 InWidth, int32 InHeight, const TArray<FLinearColor>& PaletteColors)
{
	// One texel block per ground cell over the WHOLE canvas, so the hint layer can cover any
	// canvas shape (square, landscape or portrait) with the plane simply scaled to match.
	const int32 CellsX = FMath::Clamp(InWidth, 1, 1024);
	const int32 CellsY = FMath::Clamp(InHeight, 1, 1024);

	// TexelsPerCell leaves room for crisp 1-texel grid lines between the blocks AND for a small
	// palette-number label in each block's corner. The budget keeps a 256x256 canvas at 6144 texels
	// per side (a power-of-two-friendly 36 MB texture), and larger ones get proportionally coarser
	// blocks rather than a texture the GPU cannot hold.
	constexpr int32 TargetTexelsPerSide = 6144;
	constexpr int32 MinTexelsPerCell = 8;
	const int32 TexelsPerCell = FMath::Max(MinTexelsPerCell, TargetTexelsPerSide / FMath::Max(CellsX, CellsY));
	const int32 SizeX = CellsX * TexelsPerCell;
	const int32 SizeY = CellsY * TexelsPerCell;

	if (CellColors.Num() != CellsX * CellsY || HasCell.Num() != CellsX * CellsY || CellLabels.Num() != CellsX * CellsY)
	{
		return nullptr;
	}

	FImage Image(SizeX, SizeY, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());

	// Start everything transparent.
	for (int32 i = 0; i < SizeX * SizeY; ++i)
	{
		Data[i] = FColor(0, 0, 0, 0);
	}

	// Opacity is baked per texel: the grid lines and LIGHT blocks keep their previous look
	// (alpha 204 = 0.8 effective at the default layer opacity), while DARK blocks (perceived
	// brightness <= 0.35, e.g. black / dark brown) are baked fully opaque so their colour can
	// still be told apart from the ground. The "透明度±" slider still scales the whole layer.
	constexpr uint8 LightAlpha = 204; // 0.8
	constexpr uint8 DarkAlpha = 255;  // 1.0
	constexpr float DarkLuminanceMax = 0.35f;
	const FColor Line(35, 35, 35, LightAlpha);

	for (int32 Ty = 0; Ty < SizeY; ++Ty)
	{
		const int32 GY = Ty / TexelsPerCell;
		const int32 iy = Ty % TexelsPerCell;

		for (int32 Tx = 0; Tx < SizeX; ++Tx)
		{
			const int32 GX = Tx / TexelsPerCell;
			const int32 ix = Tx % TexelsPerCell;

			// Grid: each block's top/left edge plus the canvas' outer right/bottom frame.
			const bool bGrid = (ix == 0 || iy == 0)
				|| (GX == CellsX - 1 && ix == TexelsPerCell - 1)
				|| (GY == CellsY - 1 && iy == TexelsPerCell - 1);
			if (bGrid)
			{
				Data[Ty * SizeX + Tx] = Line;
				continue;
			}

			const int32 Src = GY * CellsX + GX;
			if (HasCell[Src])
			{
				FColor C = CellColors[Src].ToFColor(true);
				const float Lum = (0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B) / 255.0f;
				C.A = (Lum <= DarkLuminanceMax) ? DarkAlpha : LightAlpha;
				Data[Ty * SizeX + Tx] = C;
			}
		}
	}

	// ---- palette-number labels (bottom-right corner of each block) ----
	// Tiny 3x5 digit font, drawn without an outline, size/stroke unchanged. The glyphs always
	// use the opposite shade of the block fill (near-black on light blocks, near-white on dark
	// ones) so they stay readable even on pale hint blocks, and are baked near-opaque so they
	// stay visible at any hint-layer opacity.
	static const char* const DigitGlyphs[10] = {
		"111101101101111", // 0
		"010110010010111", // 1
		"111001111100111", // 2
		"111001111001111", // 3
		"101101111001001", // 4
		"111100111001111", // 5
		"111100111101111", // 6
		"111001010010010", // 7
		"111101111101111", // 8
		"111101111001111", // 9
	};
	constexpr int32 FontW = 3;
	constexpr int32 FontH = 5;
	constexpr int32 FontPitch = 4; // 3 + 1 gap
	// TexelsPerCell comes from a ternary (not FMath::Max) so these stay constant-foldable.
	const int32 TextRight = TexelsPerCell - 3;  // local column of the last digit's right edge
	const int32 TextBottom = TexelsPerCell - 3; // local row of the last font row

	for (int32 By = 0; By < CellsY; ++By)
	{
		const int32 Oy = By * TexelsPerCell;
		for (int32 Bx = 0; Bx < CellsX; ++Bx)
		{
			const int32 Src = By * CellsX + Bx;
			if (!HasCell[Src]) continue;
			const int32 Label = CellLabels[Src];
			if (Label < 1 || Label > PaletteColors.Num()) continue;

			// Opposite-shade digits: near-black over light blocks, near-white over dark ones.
			FColor Core(245, 245, 245, 250);
			{
				const FColor Fill = CellColors[Src].ToFColor(true);
				const float FillLum = (0.2126f * Fill.R + 0.7152f * Fill.G + 0.0722f * Fill.B) / 255.0f;
				Core = (FillLum > 0.5f) ? FColor(10, 10, 10, 250) : FColor(250, 250, 250, 250);
			}

			char Buffer[3];
			int32 NumDigits = 0;
			if (Label >= 10)
			{
				Buffer[NumDigits++] = static_cast<char>('0' + (Label / 10));
			}
			Buffer[NumDigits++] = static_cast<char>('0' + (Label % 10));

			const int32 Ox = Bx * TexelsPerCell;
			const int32 TotalW = NumDigits * FontPitch - 1;
			const int32 StartX = Ox + TextRight - (TotalW - 1);
			const int32 StartY = Oy + TextBottom - (FontH - 1);

			for (int32 D = 0; D < NumDigits; ++D)
			{
				const char* G = DigitGlyphs[Buffer[D] - '0'];
				const int32 GlyphX = StartX + D * FontPitch;
				for (int32 Row = 0; Row < FontH; ++Row)
				{
					for (int32 Col = 0; Col < FontW; ++Col)
					{
						if (G[Row * FontW + Col] != '1') continue;
						Data[(StartY + Row) * SizeX + (GlyphX + Col)] = Core;
					}
				}
			}
		}
	}

	UTexture2D* Texture = FImageUtils::CreateTexture2DFromImage(FImageView(Image));
	if (Texture)
	{
		Texture->Filter = TF_Nearest;
		Texture->SRGB = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->UpdateResource();
	}
	return Texture;
}

bool FBeanAssets::CreateCellsImage(const TArray<uint8>& Cells, int32 Width, int32 Height, uint8 EmptyValue,
	const TArray<FLinearColor>& PaletteColors, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
	int32 PixelsPerCell, FImage& OutImage)
{
	const int32 CellsW = MaxX - MinX + 1;
	const int32 CellsH = MaxY - MinY + 1;
	const int32 Scale = FMath::Max(1, PixelsPerCell);
	const int32 ColorCount = PaletteColors.Num();
	if (CellsW <= 0 || CellsH <= 0 || ColorCount <= 0 || Width <= 0 || Height <= 0 || Cells.Num() < Width * Height)
	{
		return false;
	}

	const int32 SizeX = CellsW * Scale;
	const int32 SizeY = CellsH * Scale;
	OutImage.Init(SizeX, SizeY, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(OutImage.RawData.GetData());

	// The pegboard the beads sit on: the same warm off-white as the ground, so an exported
	// picture reads like the board the beads were placed on.
	const FColor Board(235, 232, 222);
	for (int32 i = 0; i < SizeX * SizeY; ++i)
	{
		Data[i] = Board;
	}

	// A proportional gap keeps neighbouring beads apart. It stays 0 below 8 px per bead, where
	// a gap would eat more of the bead than it shows.
	const int32 Gap = Scale / 8;
	for (int32 By = 0; By < CellsH; ++By)
	{
		for (int32 Bx = 0; Bx < CellsW; ++Bx)
		{
			const uint8 ColorIndex = Cells[(MinY + By) * Width + (MinX + Bx)];
			if (ColorIndex == EmptyValue || ColorIndex >= ColorCount)
			{
				continue;
			}

			const FColor Bead = PaletteColors[ColorIndex].ToFColor(true);
			const int32 X0 = Bx * Scale + Gap;
			const int32 Y0 = By * Scale + Gap;
			const int32 X1 = (Bx + 1) * Scale - Gap;
			const int32 Y1 = (By + 1) * Scale - Gap;
			for (int32 Y = Y0; Y < Y1; ++Y)
			{
				FColor* Row = Data + Y * SizeX;
				for (int32 X = X0; X < X1; ++X)
				{
					Row[X] = Bead;
				}
			}
		}
	}

	return true;
}

UTexture2D* FBeanAssets::CreateImageTexture(const FImage& Image)
{
	if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.RawData.Num() <= 0)
	{
		return nullptr;
	}

	UTexture2D* Texture = FImageUtils::CreateTexture2DFromImage(FImageView(Image));
	if (Texture)
	{
		// Hard pixel edges, so the on-screen preview looks like the file that will be written.
		Texture->Filter = TF_Nearest;
		Texture->SRGB = true;
		Texture->UpdateResource();
	}
	return Texture;
}
