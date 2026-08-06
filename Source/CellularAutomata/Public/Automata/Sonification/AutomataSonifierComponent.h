#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Automata/Sonification/SonificationFeatures.h"
#include "AutomataSonifierComponent.generated.h"

class AAutomataOrchestrator;
class UAudioComponent;

/** Мост между статистикой симуляции и графом MetaSound: раз в тик прочитать
 *  историю поколений, измерить форму кривой, сгладить и разослать параметры.
 *
 *  Почему ОТДЕЛЬНЫЙ компонент, а не код в AAutomataOrchestrator::Tick():
 *  тик актора у оркестратора выключен по умолчанию и включается формулой
 *  (срез || прогон идёт || быстрый шаг), разбросанной по семи местам. То есть
 *  на паузе актор не тикает вовсе - а звуку именно на паузе и надо доигрывать
 *  затухание и замечать нажатие P. Тик компонента регистрируется отдельно от
 *  PrimaryActorTick и живёт своим bTickEnabled, так что все семь мест остаются
 *  нетронутыми.
 *
 *  Создаётся в рантайме через AAutomataOrchestrator::EnsureSonifier(), а не
 *  CreateDefaultSubobject в конструкторе: Live Coding не хот-патчит добавление
 *  default-subobject'а на уже расставленных в уровне акторах (та же причина,
 *  по которой оба mesh-компонента там создаются всегда). Идиом - EnsureHeadlight().
 *
 *  СОБЫТИЯ ЛОВЯТСЯ ФРОНТАМИ, а не врезками в оркестратор, и это не экономия:
 *  - TryAutoReseedOnExtinction() не годится под вымирание в принципе - она
 *    выходит по первому же if, когда автоперекат выключен, так что с
 *    погашенным Shift+N вымирание там не детектируется никогда;
 *  - фронт по числу живых ловит и вымирание НЕ от шага - Delete на паузе,
 *    бейк, освобождающий сетку, - а врезка в шаг это пропустила бы;
 *  - фронт по счётчику поколений покрывает все пути "начать заново" одним
 *    условием, вместо разбора, кто позвал ResetGenerationCounter();
 *  - двойного выстрела не бывает по построению: фронт - это сравнение с
 *    прошлым тиком, а прямые врезки пришлось бы страховать, потому что и
 *    AppendGenerationSample(), и TryAutoReseedOnExtinction() зовутся из двух
 *    мест каждая.
 *  Цена - задержка до одного тика. Для "бум, всё погасло" она неслышна.
 *
 *  Всё строго на игровом потоке. Фоновый шаг не трогает ни компонент, ни
 *  историю поколений - как и весь остальной доступ к ней. */
UCLASS(ClassGroup = (Automata), meta = (BlueprintSpawnableComponent))
class CELLULARAUTOMATA_API UAutomataSonifierComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAutomataSonifierComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Одиночный позиционный звук клетки, в которую ткнули мышью.
	 *
	 *  Age нормируется на MaxAge - тем же числом, которым клетка красится
	 *  (AgeColorMaxAge), поэтому высота ноты совпадает с цветом. Это не
	 *  совпадение, а один и тот же параметр.
	 *
	 *  Голоса берутся из пула по кругу, а не через SpawnSoundAtLocation():
	 *  тот стартует звук немедленно, и высоту пришлось бы досылать уже
	 *  играющему - она приехала бы с опозданием на звуковой блок (~21 мс при
	 *  здешних настройках). Пул заодно бесплатно ограничивает полифонию. */
	void PlayCellClick(const FVector& WorldLocation, int32 Age, int32 MaxAge);

	/** Перечитать настройки владельца: громкость, ассеты, включённость.
	 *  Зовётся при переключении звука и при смене набора настроек. */
	void RefreshSettings();

	/** Последнее измерение - для HUD и лога. */
	const FSonificationFeatures& GetLastFeatures() const { return LastFeatures; }

private:
	/** Владелец. TWeakObjectPtr не нужен - компонент не переживает актор, но
	 *  UPROPERTY обязателен: нетегированный указатель после реинстансинга Live
	 *  Coding'ом держит мусор, а не nullptr, и это уже роняло редактор. */
	UPROPERTY(Transient)
	TObjectPtr<AAutomataOrchestrator> Orchestrator;

	/** Фон. Играет НЕПРЕРЫВНО с BeginPlay, даже в тишине: SetTriggerParameter()
	 *  у движка молча не делает ничего, если компонент не играет, и запуск по
	 *  событию терял бы ровно те триггеры, ради которых он заводился. */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> BedComponent;

	/** Пул голосов для кликов, по кругу. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> ClickVoices;

	UPROPERTY(Transient)
	int32 NextClickVoice = 0;

	UPROPERTY(Transient)
	FSonificationFeatures LastFeatures;

	// ------------------------------------------------- сглаженное состояние

	UPROPERTY(Transient)
	float SmoothedPopulation = 0.0f;

	UPROPERTY(Transient)
	float SmoothedSlope = 0.0f;

	UPROPERTY(Transient)
	float SmoothedCurvature = 0.0f;

	UPROPERTY(Transient)
	float SmoothedActivity = 0.0f;

	UPROPERTY(Transient)
	float SmoothedOscillation = 0.0f;

	UPROPERTY(Transient)
	float SmoothedDimension = 0.0f;

	UPROPERTY(Transient)
	float SmoothedRate = 0.0f;

	UPROPERTY(Transient)
	float SmoothedLiveness = 0.0f;

	// -------------------------------------------------- детекторы фронтов

	/** Без базовой линии первый тик выстрелил бы всеми триггерами сразу. */
	UPROPERTY(Transient)
	bool bHasEdgeBaseline = false;

	UPROPERTY(Transient)
	int32 LastAliveCount = 0;

	UPROPERTY(Transient)
	int64 LastGenerationCount = 0;

	UPROPERTY(Transient)
	int32 LastAutoReseedCount = 0;

	UPROPERTY(Transient)
	bool bLastRunning = false;

	/** Реальное время последней смены поколения - на нём стоит Liveness.
	 *  FPlatformTime::Seconds(), а не время мира: звук должен вести себя
	 *  одинаково при остановленном мире и при работающем. */
	double LastGenerationChangeSeconds = 0.0;

	/** Скользящее среднее интервала между замерами. Самокалибруется, поэтому
	 *  подсматривать LastDispatchGenerations у оркестратора не нужно. */
	double MeasuredSampleInterval = 0.0;

	/** Предупреждать ровно один раз за сессию, а не каждый кадр. */
	UPROPERTY(Transient)
	bool bMissingBedWarned = false;

	UPROPERTY(Transient)
	bool bBedNotPlayingWarned = false;

	AAutomataOrchestrator* ResolveOrchestrator();
	UAudioComponent* EnsureBed();
	UAudioComponent* AcquireClickVoice();
	UAudioComponent* CreateVoice(const TCHAR* Name, bool bSpatialized);

	/** Фронты -> триггеры. Зовётся до рассылки аналоговых параметров, чтобы
	 *  событие и сопровождающие его величины уехали в одном обновлении. */
	void DetectAndFireEvents(int32 AliveCount, int64 Generation, int32 AutoReseedCount,
		bool bRunning, double NowSeconds);

	void PushBedParameters(const struct FHudStats& Stats, float DeltaSeconds, double NowSeconds);

	/** Триггер с проверкой, что компонент играет: движок глотает его молча, и
	 *  без этой проверки "событие не слышно" было бы неотлаживаемо. */
	void FireTrigger(const FName& ParameterName);
};