#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "BeanGameMode.generated.h"

class ABeanGrid;

UCLASS()
class BEAN_API ABeanGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ABeanGameMode();

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY()
	TObjectPtr<ABeanGrid> Grid = nullptr;
};
