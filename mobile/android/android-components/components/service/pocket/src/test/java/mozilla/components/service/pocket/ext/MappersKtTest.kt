/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.service.pocket.ext

import mozilla.components.service.pocket.helpers.PocketTestResources
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.reflect.full.memberProperties

class MappersKtTest {
    @Test
    fun `GIVEN a PocketApiStory WHEN toPocketLocalStory is called THEN a one to one mapping is performed and timesShown is set to 0`() {
        val apiStory = PocketTestResources.apiExpectedPocketStoriesRecommendations[0]

        val result = apiStory.toPocketLocalStory()

        assertNotEquals(apiStory::class.memberProperties, result::class.memberProperties)
        assertSame(apiStory.url, result.url)
        assertSame(apiStory.title, result.title)
        assertSame(apiStory.imageUrl, result.imageUrl)
        assertSame(apiStory.publisher, result.publisher)
        assertSame(apiStory.category, result.category)
        assertSame(apiStory.timeToRead, result.timeToRead)
        assertEquals(DEFAULT_TIMES_SHOWN, result.timesShown)
    }

    @Test
    fun `GIVEN a PocketLocalStory WHEN toPocketRecommendedStory is called THEN a one to one mapping is performed`() {
        val localStory = PocketTestResources.dbExpectedPocketStory

        val result = localStory.toPocketRecommendedStory()

        assertNotEquals(localStory::class.memberProperties, result::class.memberProperties)
        assertSame(localStory.url, result.url)
        assertSame(localStory.title, result.title)
        assertSame(localStory.imageUrl, result.imageUrl)
        assertSame(localStory.publisher, result.publisher)
        assertSame(localStory.category, result.category)
        assertSame(localStory.timeToRead, result.timeToRead)
        assertEquals(localStory.timesShown, result.timesShown)
    }

    @Test
    fun `GIVEN a PocketLocalStory with no category WHEN toPocketRecommendedStory is called THEN a one to one mapping is performed and the category is set to general`() {
        val localStory = PocketTestResources.dbExpectedPocketStory.copy(category = "")

        val result = localStory.toPocketRecommendedStory()

        assertNotEquals(localStory::class.memberProperties, result::class.memberProperties)
        assertSame(localStory.url, result.url)
        assertSame(localStory.title, result.title)
        assertSame(localStory.imageUrl, result.imageUrl)
        assertSame(localStory.publisher, result.publisher)
        assertSame(DEFAULT_CATEGORY, result.category)
        assertSame(localStory.timeToRead, result.timeToRead)
        assertEquals(localStory.timesShown, result.timesShown)
    }

    @Test
    fun `GIVEN a PcoketRecommendedStory WHEN toPartialTimeShownUpdate is called THEN only the url and timesShown properties are kept`() {
        val story = PocketTestResources.clientExpectedPocketStory

        val result = story.toPartialTimeShownUpdate()

        assertNotEquals(story::class.memberProperties, result::class.memberProperties)
        assertEquals(2, result::class.memberProperties.size)
        assertSame(story.url, result.url)
        assertSame(story.timesShown, result.timesShown)
    }

    @Test
    fun `GIVEN a spoc downloaded from Internet WHEN it is converted to a local spoc THEN a one to one mapping is made`() {
        val apiStory = PocketTestResources.apiExpectedPocketSpocs[0]

        val result = apiStory.toLocalSpoc()

        assertEquals(apiStory.id, result.id)
        assertSame(apiStory.title, result.title)
        assertSame(apiStory.url, result.url)
        assertSame(apiStory.imageSrc, result.imageUrl)
        assertSame(apiStory.sponsor, result.sponsor)
        assertSame(apiStory.shim.click, result.clickShim)
        assertSame(apiStory.shim.impression, result.impressionShim)
        assertEquals(apiStory.priority, result.priority)
        assertEquals(apiStory.caps.lifetimeCount, result.lifetimeCapCount)
        assertEquals(apiStory.caps.flightCount, result.flightCapCount)
        assertEquals(apiStory.caps.flightPeriod, result.flightCapPeriod)
    }

    @Test
    fun `GIVEN a local spoc WHEN it is converted to be exposed to clients THEN a one to one mapping is made`() {
        val localStory = PocketTestResources.dbExpectedPocketSpoc

        val result = localStory.toPocketSponsoredStory()

        assertEquals(localStory.id, result.id)
        assertSame(localStory.title, result.title)
        assertSame(localStory.url, result.url)
        assertSame(localStory.imageUrl, result.imageUrl)
        assertSame(localStory.sponsor, result.sponsor)
        assertSame(localStory.clickShim, result.shim.click)
        assertSame(localStory.impressionShim, result.shim.impression)
        assertEquals(localStory.priority, result.priority)
        assertEquals(localStory.lifetimeCapCount, result.caps.lifetimeCount)
        assertEquals(localStory.flightCapCount, result.caps.flightCount)
        assertEquals(localStory.flightCapPeriod, result.caps.flightPeriod)
        assertTrue(result.caps.currentImpressions.isEmpty())
    }

    @Test
    fun `GIVEN a ContentRecommendationEntity WHEN it is converted to be exposed to clients THEN a one to one mapping is made`() {
        val recommendation = PocketTestResources.contentRecommendationEntity

        val result = recommendation.toContentRecommendation()

        assertSame(recommendation.corpusItemId, result.corpusItemId)
        assertSame(recommendation.scheduledCorpusItemId, result.scheduledCorpusItemId)
        assertSame(recommendation.url, result.url)
        assertSame(recommendation.title, result.title)
        assertSame(recommendation.excerpt, result.excerpt)
        assertSame(recommendation.topic, result.topic)
        assertSame(recommendation.publisher, result.publisher)
        assertSame(recommendation.isTimeSensitive, result.isTimeSensitive)
        assertSame(recommendation.imageUrl, result.imageUrl)
        assertEquals(recommendation.tileId, result.tileId)
        assertEquals(recommendation.receivedRank, result.receivedRank)
        assertEquals(recommendation.recommendedAt, result.recommendedAt)
        assertEquals(recommendation.impressions, result.impressions)
    }

    @Test
    fun `GIVEN a ContentRecommendationItem WHEN it is converted to the database object type THEN a one to one mapping is made`() {
        val recommendation = PocketTestResources.contentRecommendationResponseItem1
        val recommendedAt = 100L
        val result = recommendation.toContentRecommendationEntity(recommendedAt = recommendedAt)

        assertSame(recommendation.corpusItemId, result.corpusItemId)
        assertSame(recommendation.scheduledCorpusItemId, result.scheduledCorpusItemId)
        assertSame(recommendation.url, result.url)
        assertSame(recommendation.title, result.title)
        assertSame(recommendation.excerpt, result.excerpt)
        assertSame(recommendation.topic, result.topic)
        assertSame(recommendation.publisher, result.publisher)
        assertSame(recommendation.isTimeSensitive, result.isTimeSensitive)
        assertSame(recommendation.imageUrl, result.imageUrl)
        assertEquals(recommendation.tileId, result.tileId)
        assertEquals(recommendation.receivedRank, result.receivedRank)
        assertEquals(recommendedAt, result.recommendedAt)
        assertEquals(DEFAULT_TIMES_SHOWN, result.impressions)
    }

    @Test
    fun `GIVEN a ContentRecommendation WHEN it is an object type containing the times shown THEN only the corpusItemId and impressions properties are mapped`() {
        val recommendation = PocketTestResources.contentRecommendation

        val result = recommendation.toImpressions()

        assertSame(recommendation.corpusItemId, result.corpusItemId)
        assertEquals(recommendation.impressions, result.impressions)
    }

    @Test
    fun `GIVEN a SponsoredContentEntity WHEN it is converted to be exposed to clients THEN a one to one mapping is made`() {
        val entity = PocketTestResources.sponsoredContentEntity

        val result = entity.toSponsoredContent()

        assertSame(entity.url, result.url)
        assertSame(entity.title, result.title)
        assertSame(entity.clickUrl, result.callbacks.clickUrl)
        assertSame(entity.impressionUrl, result.callbacks.impressionUrl)
        assertSame(entity.imageUrl, result.imageUrl)
        assertSame(entity.domain, result.domain)
        assertSame(entity.excerpt, result.excerpt)
        assertSame(entity.sponsor, result.sponsor)
        assertSame(entity.blockKey, result.blockKey)
        assertTrue(result.caps.currentImpressions.isEmpty())
        assertEquals(entity.flightCapCount, result.caps.flightCount)
        assertEquals(entity.flightCapPeriod, result.caps.flightPeriod)
        assertEquals(entity.priority, result.priority)
    }

    @Test
    fun `GIVEN a MarsSpocsResponseItem WHEN it is converted to the database object type THEN a one to one mapping is made`() {
        val marsSpocsResponseItem = PocketTestResources.marsSpocsResponseItem
        val result = marsSpocsResponseItem.toSponsoredContentEntity()

        assertSame(marsSpocsResponseItem.url, result.url)
        assertSame(marsSpocsResponseItem.title, result.title)
        assertSame(marsSpocsResponseItem.callbacks.clickUrl, result.clickUrl)
        assertSame(marsSpocsResponseItem.callbacks.impressionUrl, result.impressionUrl)
        assertSame(marsSpocsResponseItem.imageUrl, result.imageUrl)
        assertSame(marsSpocsResponseItem.domain, result.domain)
        assertSame(marsSpocsResponseItem.excerpt, result.excerpt)
        assertSame(marsSpocsResponseItem.sponsor, result.sponsor)
        assertSame(marsSpocsResponseItem.blockKey, result.blockKey)
        assertEquals(marsSpocsResponseItem.caps.day, result.flightCapCount)
        assertEquals(DEFAULT_FLIGHT_CAP_PERIOD_IN_SECONDS, result.flightCapPeriod)
        assertEquals(marsSpocsResponseItem.ranking.priority, result.priority)
    }
}
