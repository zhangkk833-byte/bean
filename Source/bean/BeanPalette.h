#pragma once

#include "CoreMinimal.h"
#include "BeanPalette.generated.h"

/** One entry of the 48-color palette. */
USTRUCT(BlueprintType)
struct FBeanPaletteColor
{
	GENERATED_BODY()

	/** Display name (Chinese, shown in the UI palette bar). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Name;

	/** Color in linear space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Color;
};

/**
 * Palette helpers: load the 48-color palette from Config/BeanPalette.json,
 * or generate and persist the default palette when the file is missing.
 */
class BEAN_API FBeanPalette
{
public:
	static TArray<FBeanPaletteColor> LoadOrCreateDefault();

	static int32 GetPaletteSize() { return 48; }

	/** Nearest palette index for a linear-space color (Euclidean distance in linear RGB). */
	static int32 NearestColorIndex(const TArray<FBeanPaletteColor>& Palette, const FLinearColor& Color);

	/** Full path of the palette config file. */
	static FString GetConfigPath();
};
