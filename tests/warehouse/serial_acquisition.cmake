# Hundreds of Parquet writes and full disk flushes contend on the same macOS volume.
set_tests_properties(
  Acquisition.AProductWithoutDaysCoversTheListedLivesOfItsContracts
  Acquisition.AContractWithoutDaysTakesTheMinutesOfItsListedLife
  Acquisition.ADayByDayTaskPublishesVersionsInBatches
  PROPERTIES RUN_SERIAL TRUE)
