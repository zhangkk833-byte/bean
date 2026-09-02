#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "BeanCharacter.generated.h"

class UCameraComponent;

/** First-person pawn: walk on the ground, F toggles fly (hold Space/L-Ctrl for up/down). */
UCLASS()
class BEAN_API ABeanCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ABeanCharacter();

	void ToggleFly();
	bool IsFlying() const { return bIsFlying; }
	void SetFlyVerticalInput(bool bUp, bool bDown);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Bean")
	TObjectPtr<UCameraComponent> FirstPersonCamera = nullptr;

	bool bIsFlying = false;
	bool bFlyUp = false;
	bool bFlyDown = false;

	/** Max altitude in cm while flying. */
	float MaxFlyHeight = 5000.0f;
};
