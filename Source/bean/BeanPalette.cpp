#include "BeanPalette.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	struct FDefaultPaletteEntry
	{
		const TCHAR* Name;
		const TCHAR* Hex;
	};

	// 48 curated colors: hue groups first (rainbow order for wheel cycling), then neutrals.
	const FDefaultPaletteEntry DefaultPalette[] = {
		{TEXT("深红"), TEXT("#8B0000")},
		{TEXT("红"), TEXT("#FF0000")},
		{TEXT("亮红"), TEXT("#FF4444")},
		{TEXT("玫红"), TEXT("#FF1493")},
		{TEXT("粉红"), TEXT("#FF99BB")},
		{TEXT("珊瑚"), TEXT("#FF7F50")},
		{TEXT("深橙"), TEXT("#CC5500")},
		{TEXT("橙"), TEXT("#FF7F00")},
		{TEXT("亮橙"), TEXT("#FFA64D")},
		{TEXT("橙黄"), TEXT("#FFB300")},
		{TEXT("深黄"), TEXT("#CCAA00")},
		{TEXT("黄"), TEXT("#FFD400")},
		{TEXT("亮黄"), TEXT("#FFF176")},
		{TEXT("黄绿"), TEXT("#9ACD32")},
		{TEXT("深绿"), TEXT("#1E7A1E")},
		{TEXT("绿"), TEXT("#00A000")},
		{TEXT("亮绿"), TEXT("#66CC66")},
		{TEXT("薄荷绿"), TEXT("#98FB98")},
		{TEXT("青绿"), TEXT("#008080")},
		{TEXT("深青"), TEXT("#005F5F")},
		{TEXT("青"), TEXT("#00C8C8")},
		{TEXT("亮青"), TEXT("#99FFFF")},
		{TEXT("湖蓝"), TEXT("#00BFFF")},
		{TEXT("天蓝"), TEXT("#87CEFA")},
		{TEXT("蓝"), TEXT("#1E90FF")},
		{TEXT("亮蓝"), TEXT("#66B2FF")},
		{TEXT("深蓝"), TEXT("#1F3A93")},
		{TEXT("藏蓝"), TEXT("#0B1D4F")},
		{TEXT("蓝紫"), TEXT("#7B68EE")},
		{TEXT("深紫"), TEXT("#4B0082")},
		{TEXT("紫"), TEXT("#8A2BE2")},
		{TEXT("亮紫"), TEXT("#B266FF")},
		{TEXT("紫罗兰"), TEXT("#EE82EE")},
		{TEXT("品红"), TEXT("#FF00FF")},
		{TEXT("洋红"), TEXT("#C71585")},
		{TEXT("深棕"), TEXT("#4A2A0A")},
		{TEXT("棕"), TEXT("#8B5A2B")},
		{TEXT("浅棕"), TEXT("#C19A6B")},
		{TEXT("米黄"), TEXT("#EEDC82")},
		{TEXT("金"), TEXT("#FFD700")},
		{TEXT("白"), TEXT("#FFFFFF")},
		{TEXT("亮灰"), TEXT("#D0D0D0")},
		{TEXT("银灰"), TEXT("#C0C0C0")},
		{TEXT("灰"), TEXT("#808080")},
		{TEXT("深灰"), TEXT("#404040")},
		{TEXT("碳灰"), TEXT("#303030")},
		{TEXT("墨色"), TEXT("#1A1A1A")},
		{TEXT("黑"), TEXT("#000000")},
	};

	constexpr int32 NumDefaultColors = sizeof(DefaultPalette) / sizeof(DefaultPalette[0]);

	TArray<FBeanPaletteColor> BuildDefaults()
	{
		TArray<FBeanPaletteColor> Result;
		Result.Reserve(NumDefaultColors);
		for (const FDefaultPaletteEntry& Entry : DefaultPalette)
		{
			FBeanPaletteColor Item;
			Item.Name = Entry.Name;
			const FColor Srgb = FColor::FromHex(Entry.Hex);
			Item.Color = FLinearColor::FromSRGBColor(Srgb);
			Result.Add(Item);
		}
		return Result;
	}

	FString ColorToHex(const FLinearColor& Color)
	{
		const FColor Srgb = Color.ToFColor(true);
		return FString::Printf(TEXT("#%02X%02X%02X"), Srgb.R, Srgb.G, Srgb.B);
	}
}

FString FBeanPalette::GetConfigPath()
{
	return FPaths::ProjectConfigDir() / TEXT("BeanPalette.json");
}

TArray<FBeanPaletteColor> FBeanPalette::LoadOrCreateDefault()
{
	const TArray<FBeanPaletteColor> Defaults = BuildDefaults();
	const FString Path = GetConfigPath();

	TArray<FBeanPaletteColor> Result;
	FString Json;
	if (FFileHelper::LoadFileToString(Json, *Path))
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Colors = nullptr;
			if (Root->TryGetArrayField(TEXT("colors"), Colors))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Colors)
				{
					const TSharedPtr<FJsonObject> Obj = Value->AsObject();
					if (!Obj.IsValid()) continue;
					FString Name;
					FString Hex;
					if (!Obj->TryGetStringField(TEXT("name"), Name)) continue;
					if (!Obj->TryGetStringField(TEXT("hex"), Hex)) continue;
					FBeanPaletteColor Item;
					Item.Name = Name;
					const FColor Srgb = FColor::FromHex(Hex);
					Item.Color = FLinearColor::FromSRGBColor(Srgb);
					Result.Add(Item);
				}
			}
		}
	}

	if (Result.Num() == NumDefaultColors)
	{
		return Result;
	}

	// Missing or invalid config: write the default palette file.
	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> ColorArray;
	for (const FBeanPaletteColor& Item : Defaults)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Item.Name);
		Obj->SetStringField(TEXT("hex"), ColorToHex(Item.Color));
		ColorArray.Add(MakeShareable(new FJsonValueObject(Obj)));
	}
	Root->SetArrayField(TEXT("colors"), ColorArray);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(OutJson, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	return Defaults;
}

int32 FBeanPalette::NearestColorIndex(const TArray<FBeanPaletteColor>& Palette, const FLinearColor& Color)
{
	if (Palette.Num() == 0) return 0;

	int32 BestIndex = 0;
	float BestDistance = TNumericLimits<float>::Max();
	for (int32 i = 0; i < Palette.Num(); ++i)
	{
		const FLinearColor& C = Palette[i].Color;
		const float Dr = C.R - Color.R;
		const float Dg = C.G - Color.G;
		const float Db = C.B - Color.B;
		const float Distance = Dr * Dr + Dg * Dg + Db * Db;
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			BestIndex = i;
		}
	}
	return BestIndex;
}
