#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "MomentumCharacterBase.generated.h"

class UCameraComponent;
class UCapsuleComponent;
class UMomentumMovementComponent;
class USkeletalMeshComponent;

/**
 * Base first person pawn for Momentum.
 *
 * "Character" is used instead of "Character" on purpose: this pawn deliberately does not derive from
 * ACharacter, so naming it a character would imply an inheritance and a movement component it does
 * not have. It is built directly on APawn with UMomentumMovementComponent.
 *
 * Responsibilities are split so that input and simulation never reach across each other:
 *
 *   AMomentumCharacterBase  owns the body (capsule, view, mesh, movement) and exposes an intent API.
 *   AMomentumCharacterHero  translates player input into calls on that intent API.
 *   UMomentumMovementComponent  simulates movement and knows nothing about either class.
 *
 * Anything that can drive intent can drive this pawn: a player controller, an AI behaviour tree, a
 * cutscene, or a replay. Subclass it for a shared body that is not the player's.
 *
 * Component layout:
 *   CapsuleComponent (root, collision, the thing the movement component moves)
 *     |- ViewPivot        vertical eye anchor, interpolated on crouch
 *     |   \- CameraComponent
 *     \- MeshComponent    third person body and shadow
 */
UCLASS(Abstract, meta = (DisplayName = "Momentum Character Base"))
class MOMENTUM_API AMomentumCharacterBase : public APawn
{
	GENERATED_BODY()

public:
	AMomentumCharacterBase();

	//~ Begin AActor interface
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor interface

	//~ Begin APawn interface
	virtual UPawnMovementComponent* GetMovementComponent() const override;
	virtual FVector GetPawnViewLocation() const override;
	//~ End APawn interface

	// ---------------------------------------------------------------------------------------------
	// Intent API. This is the whole surface a controller, AI or ability needs. Nothing here reads
	// input devices, so any driver can use it.
	// ---------------------------------------------------------------------------------------------

	/** Move relative to the current view. X is strafe, Y is forward, magnitude 0..1. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void AddMoveInput(FVector2D MoveInput);

	/** Turn the view. X is yaw, Y is pitch, both already scaled by the caller. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void AddLookInput(FVector2D LookInput);

	/** Press jump. Buffered by the movement component, so pressing slightly early still works. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void StartJump();

	/** Release jump. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void StopJump();

	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void SetSprinting(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void SetCrouching(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Momentum|Character|Intent")
	void ToggleCrouch();

	// ---------------------------------------------------------------------------------------------
	// Accessors
	// ---------------------------------------------------------------------------------------------

	/** Typed accessor for the movement component, so callers avoid casting GetMovementComponent(). */
	UFUNCTION(BlueprintPure, Category = "Momentum|Character")
	UMomentumMovementComponent* GetMomentumMovement() const { return MovementComponent; }

	UFUNCTION(BlueprintPure, Category = "Momentum|Character")
	UCapsuleComponent* GetCapsuleComponent() const { return CapsuleComponent; }

	UFUNCTION(BlueprintPure, Category = "Momentum|Character")
	UCameraComponent* GetCameraComponent() const { return CameraComponent; }

	UFUNCTION(BlueprintPure, Category = "Momentum|Character")
	USkeletalMeshComponent* GetMeshComponent() const { return MeshComponent; }

	/** Eye height above the pawn's feet right now, mid-interpolation included. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Character|View")
	float GetCurrentViewHeight() const { return CurrentViewHeight; }

protected:
	/** Smooth the eye height toward the standing or crouched target. Called every frame. */
	virtual void UpdateViewHeight(float DeltaSeconds);

	// ---------------------------------------------------------------------------------------------
	// View tuning
	// ---------------------------------------------------------------------------------------------

	/** Eye height above the pawn's feet while standing. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Momentum|Character|View", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "Centimeters"))
	float StandingViewHeight = 160.0f;

	/** Eye height above the pawn's feet while crouched. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Momentum|Character|View", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "Centimeters"))
	float CrouchedViewHeight = 80.0f;

	/**
	 * How quickly the eye moves between the two heights. The capsule resizes instantly, but because
	 * the eye is positioned from the feet rather than from the capsule, interpolating this value
	 * alone gives a continuous view with no snap on the transition frame.
	 * Zero disables smoothing.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Momentum|Character|View", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float ViewHeightInterpSpeed = 10.0f;

private:
	/** Collision capsule and root. This is the component the movement component actually moves. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Momentum|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCapsuleComponent> CapsuleComponent;

	/** Vertical anchor for the camera, driven by UpdateViewHeight. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Momentum|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> ViewPivot;

	/** First person camera. Takes its rotation from the controller. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Momentum|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> CameraComponent;

	/** Body mesh, present for shadows and third person views. Collision is off by default. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Momentum|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> MeshComponent;

	/** The movement simulation. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Momentum|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMomentumMovementComponent> MovementComponent;

	/** Interpolated eye height above the feet, in centimetres. */
	float CurrentViewHeight = 160.0f;
};