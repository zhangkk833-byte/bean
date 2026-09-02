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

	// -- input handlers ---------------------------------------------------
	void OnMoveForward(const FInputActionValue& Value);
	void OnMoveRight(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnPlace(const FInputActionValue& Value);
	void OnRemove(const FInputActionValue& Value);
	void OnCycleColor(const FInputActionValue& Value);
	void OnToggleFly(const FInputActionValue& Value);
	void OnFlyUp(const FInputActionValue& Value);
	void OnFlyDown(const FInputActionValue& Value);
	void OnToggleCursor(const FInputActionValue& Value);
	void OnToggleUI(const FInputActionValue& Value);
	void OnSave(const FInputActionValue& Value);

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

	float LookSensitivity = 0.5f;
};
