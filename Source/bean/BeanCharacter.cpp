#include "BeanCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

ABeanCharacter::ABeanCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(30.0f, 90.0f);
	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(GetCapsuleComponent());
	FirstPersonCamera->SetRelativeLocation(FVector(0.0f, 0.0f, 70.0f)); // eye height 1.6m
	FirstPersonCamera->bUsePawnControlRotation = true;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxStepHeight = 28.0f; // step over 0.25m cubes
		Movement->bOrientRotationToMovement = false;
	}
}

void ABeanCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = 300.0f;   // 3 m/s (12 cells/s)
		Movement->MaxFlySpeed = 750.0f;    // 7.5 m/s (30 cells/s)
		// Snappy fly: high acceleration reaches speed fast; high braking stops
		// rising/falling immediately when the key is released.
		Movement->MaxAcceleration = 20000.0f;
		Movement->BrakingDecelerationFlying = 100000.0f;
	}
}

void ABeanCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bIsFlying)
	{
		AddMovementInput(FVector::UpVector, (bFlyUp ? 1.0f : 0.0f) - (bFlyDown ? 1.0f : 0.0f));

		FVector Location = GetActorLocation();
		if (Location.Z > MaxFlyHeight)
		{
			Location.Z = MaxFlyHeight;
			SetActorLocation(Location);
		}
		if (Location.Z < 0.0f)
		{
			Location.Z = 0.0f;
			SetActorLocation(Location);
		}
	}
}

void ABeanCharacter::ToggleFly()
{
	bIsFlying = !bIsFlying;
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		if (bIsFlying)
		{
			Movement->SetMovementMode(MOVE_Flying);
			Movement->Velocity.Z = 0.0f;
		}
		else
		{
			Movement->SetMovementMode(MOVE_Walking);
		}
	}
}

void ABeanCharacter::SetFlyVerticalInput(bool bUp, bool bDown)
{
	bFlyUp = bUp;
	bFlyDown = bDown;
}
