#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BeanPalette.h"
#include "BeanHUDWidget.generated.h"

class ABeanPlayerController;
class UBorder;
class UButton;
class UCanvasPanel;
class UHorizontalBox;
class UImage;
class UTextBlock;
class UTexture2D;
class UVerticalBox;

/** One entry of the bottom palette bar: a 3D-cube icon + its color label. */
UCLASS()
class BEAN_API UBeanPaletteItemWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Configure before the widget is added to the viewport tree. */
	void Setup(int32 InColorIndex, const FLinearColor& InColor, const FString& InName, UTexture2D* InIcon, float InItemWidth, float InItemHeight);

	TFunction<void(int32)> OnClicked;

	void SetSelected(bool bSelected);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleButtonClicked();

	void BuildContent();

	UPROPERTY()
	TObjectPtr<UButton> Button = nullptr;
	UPROPERTY()
	TObjectPtr<UImage> Icon = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> Label = nullptr;
	UPROPERTY()
	TObjectPtr<UBorder> Frame = nullptr;

	int32 ColorIndex = 0;
	FLinearColor Color = FLinearColor::White;
	FString ColorName;
	TObjectPtr<UTexture2D> IconTexture = nullptr;
	float ItemWidth = 40.0f;
	float ItemHeight = 66.0f;
};

/** Full-screen HUD: palette bar, hint texts, import/overlay buttons, crosshair. */
UCLASS()
class BEAN_API UBeanHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetController(ABeanPlayerController* InController) { Controller = InController; }

	void SetSelectedColor(int32 ColorIndex);
	void SetImportButtonsEnabled(bool bHasOverlay);
	void ToggleVisibility();

	/** Invoked by palette item clicks. */
	TFunction<void(int32)> OnColorSelected;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildUI();
	void BuildPaletteBar();
	UCanvasPanel* GetRootCanvas();

	UFUNCTION()
	void HandleImportClicked();
	UFUNCTION()
	void HandleApplyClicked();
	UFUNCTION()
	void HandleCancelClicked();
	UFUNCTION()
	void HandleOpacityIncClicked();
	UFUNCTION()
	void HandleOpacityDecClicked();
	UFUNCTION()
	void HandleToggleOverlayClicked();
	UFUNCTION()
	void HandleExitClicked();
	UFUNCTION()
	void HandleToggleFullscreenClicked();

	UPROPERTY()
	TObjectPtr<UCanvasPanel> RootCanvas = nullptr;
	UPROPERTY()
	TObjectPtr<UVerticalBox> PaletteBar = nullptr;
	UPROPERTY()
	TObjectPtr<UVerticalBox> ButtonStack = nullptr;
	UPROPERTY()
	TObjectPtr<UImage> Crosshair = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> ApplyButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> CancelButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> ToggleOverlayButton = nullptr;
	UPROPERTY()
	TArray<TObjectPtr<UBeanPaletteItemWidget>> PaletteItems;

	TObjectPtr<ABeanPlayerController> Controller = nullptr;
	bool bBuiltPaletteBar = false;
};
