#include "MomentumCharacterHero.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputActionValue.h"
#include "Momentum/Momentum.h"

AMomentumCharacterHero::AMomentumCharacterHero()
{
	// Use this pawn's camera directly when it becomes the view target, rather than a separate
	// camera actor.
	bFindCameraComponentWhenViewTarget = true;
}

void AMomentumCharacterHero::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// Runs on possession and on unpossession; the helper handles both by checking the controller.
	AddDefaultMappingContext();
}

void AMomentumCharacterHero::AddDefaultMappingContext()
{
	if (!DefaultMappingContext)
	{
		return;
	}

	const APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (!PlayerController)
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(DefaultMappingContext, MappingContextPriority);
	}
}

void AMomentumCharacterHero::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogMomentum, Error,
			TEXT("%s expects an Enhanced Input component. Set the project's Default Input Component Class to EnhancedInputComponent."),
			*GetNameSafe(this));
		return;
	}

	if (MoveAction)
	{
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMomentumCharacterHero::Input_Move);
	}

	if (LookAction)
	{
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &AMomentumCharacterHero::Input_Look);
	}

	if (JumpAction)
	{
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this, &AMomentumCharacterHero::Input_JumpStarted);
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Completed, this, &AMomentumCharacterHero::Input_JumpCompleted);
	}

	if (CrouchAction)
	{
		EnhancedInput->BindAction(CrouchAction, ETriggerEvent::Started, this, &AMomentumCharacterHero::Input_CrouchStarted);
		EnhancedInput->BindAction(CrouchAction, ETriggerEvent::Completed, this, &AMomentumCharacterHero::Input_CrouchCompleted);
	}

	if (SprintAction)
	{
		EnhancedInput->BindAction(SprintAction, ETriggerEvent::Started, this, &AMomentumCharacterHero::Input_SprintStarted);
		EnhancedInput->BindAction(SprintAction, ETriggerEvent::Completed, this, &AMomentumCharacterHero::Input_SprintCompleted);
	}
}

void AMomentumCharacterHero::Input_Move(const FInputActionValue& Value)
{
	AddMoveInput(Value.Get<FVector2D>());
}

void AMomentumCharacterHero::Input_Look(const FInputActionValue& Value)
{
	const FVector2D LookAxis = Value.Get<FVector2D>();

	AddLookInput(FVector2D(
		LookAxis.X * LookSensitivity.X,
		LookAxis.Y * LookSensitivity.Y * (bInvertLookY ? -1.0f : 1.0f)));
}

void AMomentumCharacterHero::Input_JumpStarted(const FInputActionValue& Value)
{
	StartJump();
}

void AMomentumCharacterHero::Input_JumpCompleted(const FInputActionValue& Value)
{
	StopJump();
}

void AMomentumCharacterHero::Input_CrouchStarted(const FInputActionValue& Value)
{
	if (bToggleCrouch)
	{
		ToggleCrouch();
	}
	else
	{
		SetCrouching(true);
	}
}

void AMomentumCharacterHero::Input_CrouchCompleted(const FInputActionValue& Value)
{
	if (!bToggleCrouch)
	{
		SetCrouching(false);
	}
}

void AMomentumCharacterHero::Input_SprintStarted(const FInputActionValue& Value)
{
	SetSprinting(true);
}

void AMomentumCharacterHero::Input_SprintCompleted(const FInputActionValue& Value)
{
	SetSprinting(false);
}