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
	// No persistent cache: each Get*() re-resolves LoadObject / rebuilds.
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

UTexture2D* FBeanAssets::CreateGridTexture()
{
	// 8192x8192 over a 64m plane => 0.25m per cell, 32 texels per cell.
	// A 1-texel line is therefore ~0.8cm => very thin grid lines.
	constexpr int32 Size = 8192;
	constexpr int32 CellTexels = 32; // 32 texels = 0.25m (one grid cell)
	FImage Image(Size, Size, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());

	// Warm off-white ground with a very subtle warm-gray grid line.
	const FColor Fill(235, 232, 222);
	const FColor Line(220, 216, 205);
	const FColor Border(196, 192, 182);

	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			FColor C = Fill;
			if (X == 0 || Y == 0 || X == Size - 1 || Y == Size - 1)
			{
				C = Border;
			}
			else if ((X % CellTexels == 0) || (Y % CellTexels == 0))
			{
				C = Line;
			}
			Data[Y * Size + X] = C;
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

UTexture2D* FBeanAssets::CreateCubeIconTexture(const FLinearColor& Color, int32 IconSize)
{
	const int32 Size = FMath::Max(16, IconSize);
	FImage Image(Size, Size, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());
	for (int32 i = 0; i < Size * Size; ++i)
	{
		Data[i] = FColor(0, 0, 0, 0);
	}

	// A standard cube: a large square FRONT face plus narrow top and right faces
	// that lean up-and-right. Short lean keeps the whole silhouette square (1:1)
	// instead of looking tall and thin.
	const float Half = Size * 0.36f;    // front face half size (square)
	const float LeanX = Size * 0.13f;   // top/right faces lean right
	const float LeanY = Size * 0.13f;   // top/right faces lean up
	// Center the whole silhouette (right/up lean is asymmetric, so nudge the front).
	const float Cx = Size * 0.5f - LeanX * 0.5f;
	const float Cy = Size * 0.5f + LeanY * 0.5f;

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

	// Crisp outer outline: any opaque pixel adjacent to a transparent one.
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			FColor& C = Data[Y * Size + X];
			if (C.A == 0) continue;
			const bool bEdge =
				Data[Y * Size + (X - 1)].A == 0 ||
				Data[Y * Size + (X + 1)].A == 0 ||
				Data[(Y - 1) * Size + X].A == 0 ||
				Data[(Y + 1) * Size + X].A == 0;
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

UTexture2D* FBeanAssets::CreateOverlayTexture(const TArray<FLinearColor>& CellColors, const TArray<bool>& HasCell, int32 Granularity)
{
	// Always build a full 256x256 texture so sampling behavior is identical at every
	// granularity (a small N x N texture enlarged near the camera made the preview
	// flicker/alias). The N x N data is written into the CENTER of the texture.
	constexpr int32 Size = 256;
	const int32 N = FMath::Clamp(Granularity, 1, Size);
	FImage Image(Size, Size, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FColor* Data = reinterpret_cast<FColor*>(Image.RawData.GetData());

	// Start everything transparent.
	for (int32 i = 0; i < Size * Size; ++i)
	{
		Data[i] = FColor(0, 0, 0, 0);
	}

	const int32 Start = (Size - N) / 2;
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 Src = Y * N + X;
			if (Src < HasCell.Num() && HasCell[Src] && Src < CellColors.Num())
			{
				FColor C = CellColors[Src].ToFColor(true);
				C.A = 255;
				Data[(Start + Y) * Size + (Start + X)] = C;
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
