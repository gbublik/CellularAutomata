// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/CameraManager/GameCameraManager.h"
#include "Camera/CameraTypes.h"

void AGameCameraManager::UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime)
{
	Super::UpdateViewTarget(OutVT, DeltaTime);

	if (!bOrthographic)
	{
		// Ничего не переопределяем - вид остаётся ровно тем, что построил
		// базовый класс, т.е. обычной перспективой.
		return;
	}

	OutVT.POV.ProjectionMode = ECameraProjectionMode::Orthographic;
	OutVT.POV.OrthoWidth = OrthoWidth;

	// Планы отсечения задаются явно: авторасчёт (bAutoCalculateOrthoPlanes, в
	// FMinimalViewInfo включён по умолчанию) выводит их из расстояния до цели
	// вида, которое этот проект нигде не задаёт - обрезка получилась бы
	// непредсказуемой.
	//
	// Слой симметричен относительно камеры (ближний план ОТРИЦАТЕЛЬНЫЙ), а не
	// начинается от неё: ортопроекцию включают, чтобы разглядеть структуру, и
	// камера при этом часто оказывается внутри неё - с ближним планом 0 всё,
	// что за камерой, просто исчезло бы, а это читается как поломка, а не как
	// смена проекции. Ортовиды DCC-редакторов ведут себя так же.
	//
	// Диапазон намеренно конечный (метр точности не нужен, но и UE_OLD_WORLD_MAX
	// брать незачем): при линейной глубине ортопроекции миллион единиц на
	// float даёт шаг около десятой доли единицы, т.е. в тысячу раз мельче
	// клетки - z-fighting'а на гранях кубов не будет.
	OutVT.POV.bAutoCalculateOrthoPlanes = false;
	OutVT.POV.OrthoNearClipPlane = -OrthoDepthRange * 0.5f;
	OutVT.POV.OrthoFarClipPlane = OrthoDepthRange * 0.5f;
}

void AGameCameraManager::SetOrthoWidth(float NewOrthoWidth)
{
	OrthoWidth = FMath::Clamp(NewOrthoWidth, MinOrthoWidth, MaxOrthoWidth);
}

void AGameCameraManager::ScaleOrthoWidth(float Multiplier)
{
	SetOrthoWidth(OrthoWidth * Multiplier);

	// Флаг поднимается ЗДЕСЬ, а не в SetOrthoWidth(): через сеттер идёт и
	// подгонка под сетку при кадрировании, а она как раз не должна означать
	// "пользователь выбрал масштаб сам" - иначе первое же кадрирование
	// заблокировало бы все последующие (см. HasUserOrthoWidth()).
	bOrthoWidthUserSet = true;
}