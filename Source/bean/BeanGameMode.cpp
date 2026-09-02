#include "BeanGameMode.h"

#include "BeanCharacter.h"
#include "BeanGrid.h"
#include "BeanPlayerController.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

ABeanGameMode::ABeanGameMode()
{
	DefaultPawnClass = ABeanCharacter::StaticClass();
	PlayerControllerClass = ABeanPlayerController::StaticClass();
}

void ABeanGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (!Grid)
	{
		FActorSpawnParameters SpawnParams;
		Grid = GetWorld()->SpawnActor<ABeanGrid>(ABeanGrid::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	}

	if (ABeanPlayerController* PC = Cast<ABeanPlayerController>(UGameplayStatics::GetPlayerController(GetWorld(), 0)))
	{
		PC->SetGrid(Grid);
	}
}
