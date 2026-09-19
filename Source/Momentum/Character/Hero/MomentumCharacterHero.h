#pragma once

#include "CoreMinimal.h"
#include "Momentum/Character/MomentumCharacterBase.h"

#include "MomentumCharacterHero.generated.h"

class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/**
 * The player controlled Character.
 *
 * This class exists only to turn Enhanced Input into calls on AMomentumCharacterBase's intent API. It
 * holds no movement logic of its own, which keeps the simulation testable and drivable from AI or
 * automation without an input device anywhere in the picture.
 *
 * Designer setup:
 *   1. Create a Blueprint subclass of this pawn.
 *   2. Assign DefaultMappingContext and the five input actions in Class Defaults.
 *   3. Tune feel on the Movement Component, and the view on this pawn's Momentum|Character|View category.
 *
 * MoveAction and LookAction expect Axis2D values; the rest expect Digital (bool) values.
 */
UCLASS(meta = (DisplayName = "Momentum Character Hero"))
class MOMENTUM_API AMomentumCharacterHero : public AMomentumCharacterBase
{
	GENERATED_BODY()

public:
	AMomentumCharacterHero();

	//~ Begin APawn interface
	virtual void NotifyControllerChanged() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	//~ End APawn interface

protected:
	// --- Input bindings ---

	/** Mapping context added for this pawn's local player when it is possessed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	/** Priority of DefaultMappingContext. Higher priority contexts win conflicting bindings. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	int32 MappingContextPriority = 0;

	/** Axis2D. X strafes, Y moves forward. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	TObjectPtr<UInputAction> MoveAction;

	/** Axis2D. X yaws, Y pitches. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	TObjectPtr<UInputAction> LookAction;

	/** Digital. Held state drives auto hop when the movement component has it enabled. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	TObjectPtr<UInputAction> JumpAction;

	/** Digital. Hold or toggle depending on bToggleCrouch. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	TObjectPtr<UInputAction> CrouchAction;

	/** Digital. Hold to sprint. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Momentum|Input")
	TObjectPtr<UInputAction> SprintAction;

	// --- Look tuning ---

	/** Per-axis multiplier on look input. Keep engine-side modifiers off so this stays the one knob. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Momentum|Input|Look", meta = (ClampMin = "0.0", UIMin = "0.0"))
	FVector2D LookSensitivity = FVector2D(1.0f, 1.0f);

	/** Invert the vertical look axis. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Momentum|Input|Look")
	bool bInvertLookY = false;

	// --- Behaviour ---

	/** Crouch toggles on press instead of being held. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Momentum|Input")
	bool bToggleCrouch = false;

	// --- Handlers ---

	void Input_Move(const FInputActionValue& Value);
	void Input_Look(const FInputActionValue& Value);
	void Input_JumpStarted(const FInputActionValue& Value);
	void Input_JumpCompleted(const FInputActionValue& Value);
	void Input_CrouchStarted(const FInputActionValue& Value);
	void Input_CrouchCompleted(const FInputActionValue& Value);
	void Input_SprintStarted(const FInputActionValue& Value);
	void Input_SprintCompleted(const FInputActionValue& Value);

private:
	/** Add DefaultMappingContext to the local player's Enhanced Input subsystem, if there is one. */
	void AddDefaultMappingContext();
};