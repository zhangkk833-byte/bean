#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "BeanPlayerController.generated.h"

class ABeanCharacter;
class ABeanGrid;
class UBeanHUDWidget;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

UCLASS()
class BEAN_API ABeanPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ABeanPlayerController();

	void SetGrid(ABeanGrid* InGrid) { Grid = InGrid; }

	// -- called by the HUD -----------------------------------------------
	void ImportImage();
	void ApplyImage();
	void CancelImage();
	void ChangeOverlayOpacity(float Delta);
	void ToggleOverlayVisible();
	void CycleColor(int32 Delta);
	void SetCurrentColor(int32 Index);
	/** Re-assert the current mouse-cursor/input mode. The 全屏 button and the import picker both
	 *  hand the OS focus around, which drops the mouse capture and otherwise leaves camera look
	 *  and the HUD cursor wedged. */
	void ReapplyCursorMode() { ApplyCursorMode(); }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupInputComponent() override;

private:
	void BindEnhancedInput();
	void AddMappingContext();
	void ApplyCursorMode();
	void UpdateTarget();
	void EnsureGrid();
	/** True when the widget under the mouse cursor belongs to the HUD (buttons / palette cubes). */
	bool IsClickOverHUD() const;
	/** Modal window asking the user to pick a preview granularity (32/64/128/256). Returns the choice, or 0 if cancelled. */
	int32 PromptGranularity();
	/** Deferred import step: run the native file picker, then load the chosen image. */
	void OpenImportPicker();

	// -- input handlers ---------------------------------------------------
	void OnMoveForward(const FInputActionValue& Value);
	void OnMoveRight(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnPlace(const FInputActionValue& Value);
	void OnPlaceHeld(const FInputActionValue& Value);
	void OnPlaceReleased(const FInputActionValue& Value);
	void OnRemove(const FInputActionValue& Value);
	void OnRemoveHeld(const FInputActionValue& Value);
	void OnRemoveReleased(const FInputActionValue& Value);
	void OnCycleColor(const FInputActionValue& Value);
	void OnToggleFly(const FInputActionValue& Value);
	void OnFlyUp(const FInputActionValue& Value);
	void OnFlyDown(const FInputActionValue& Value);
	void OnToggleCursor(const FInputActionValue& Value);
	void OnToggleUI(const FInputActionValue& Value);
	void OnSave(const FInputActionValue& Value);

	/** While a button is held, act on every cell swept from the last acted cell to (ToX, ToY). */
	void SweepPlace(int32 ToX, int32 ToY);
	void SweepRemove(int32 ToX, int32 ToY);

	UPROPERTY()
	TObjectPtr<UInputMappingContext> MappingContext = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_MoveForward = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_MoveRight = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Look = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Place = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Remove = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_CycleColor = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_ToggleFly = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_FlyUp = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_FlyDown = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_ToggleCursor = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_ToggleUI = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Save = nullptr;

	UPROPERTY()
	TObjectPtr<ABeanGrid> Grid = nullptr;
	UPROPERTY()
	TObjectPtr<UBeanHUDWidget> HUDWidget = nullptr;
	UPROPERTY()
	TObjectPtr<ABeanCharacter> PawnChar = nullptr;

	int32 CurrentColor = 1;
	int32 TargetX = 0;
	int32 TargetY = 0;
	bool bTargetValid = false;
	bool bFlyUpHeld = false;
	bool bFlyDownHeld = false;

	// -- held place/remove state (continuous sweep placement) ---------------
	/** Last grid cell acted on while the button is held (-1 = none). */
	int32 LastPlaceCellX = -1;
	int32 LastPlaceCellY = -1;
	int32 LastRemoveCellX = -1;
	int32 LastRemoveCellY = -1;
	/** True when the current press started in pointer (cursor) mode and was consumed by
	 *  exiting it — the rest of that press must not place/remove (Q2). */
	bool bPlacePressConsumed = false;
	bool bRemovePressConsumed = false;

	// -- deferred import flow ----------------------------------------------
	/** True while the deferred import flow is pending or its file picker is open, so a second
	 *  click on 导入图片 cannot queue a second dialog. Cleared as soon as the picker returns. */
	bool bImportPickerOpen = false;

	// ~1000 eDPI at 800 DPI (mouse sensitivity tuned for faster look).
	float LookSensitivity = 1.25f;
};
